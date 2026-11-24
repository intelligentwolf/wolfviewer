/**
 * @file wolffloaterrecorder.cpp
 * @brief WolfViewer: the Video Recorder window. See wolffloaterrecorder.h and floater_wolf_recorder.xml.
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
#include "llviewerprecompiledheaders.h"
#include "wolffloaterrecorder.h"

#include <cmath>
#include <cstring>
#include <filesystem>

#include "fsyspath.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "lldir.h"
#include "lldirpicker.h"
#include "llfloaterreg.h"
#include "llfontgl.h"
#include "llfocusmgr.h"
#include "llimage.h"
#include "lllocalcliprect.h"
#include "llrender2dutils.h"
#include "llscrollcontainer.h"
#include "llsliderctrl.h"
#include "lltooltip.h"
#include "lltextbox.h"
#include "lluicolortable.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"
#include "llviewertexture.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include "wolfrecorder.h"
#include "wolfvrstream.h"

// [W111 RECORDER UI 2026-11-24] The transport buttons: a big round button with its word beside it, drawn here (like a
// camcorder's or OBS Studio's record control), still an LLButton for clicks, the keyboard (Space/Enter when focused),
// tooltips and LEAP (LLFloaterRegListener clickButton finds it as an LLButton). Its own Params type: two widgets
// registered with one Params type crash at start (lluictrlfactory.cpp registerWidget).
class WolfRecorderTransportButton : public LLButton
{
public:
    struct Params : public LLInitParam::Block<Params, LLButton::Params>
    {
        Params() {}
    };
    enum Icon { ICON_RECORD, ICON_STOP, ICON_PAUSE, ICON_RESUME };
    void setIcon(Icon icon) { mIcon = icon; }
    void draw() override;

protected:
    WolfRecorderTransportButton(const Params& p) : LLButton(p) {}
    friend class LLUICtrlFactory;

private:
    Icon mIcon = ICON_RECORD;
};
static LLDefaultChildRegistry::Register<WolfRecorderTransportButton> r_wolf_recorder_transport_button("wolf_recorder_transport_button");

namespace
{
    // The monitor is dark in every skin, as a camera's screen is: its colours are fixed here, so its contrast is the
    // same in light and dark skins (each text colour is 6:1 or more against kMonBg).
    const LLColor4 kMonBg(0.063f, 0.075f, 0.090f, 1.f);      // #101317
    const LLColor4 kMonEdge(0.165f, 0.188f, 0.220f, 1.f);    // #2A3038
    const LLColor4 kMonText(0.95f, 0.95f, 0.95f, 1.f);
    const LLColor4 kMonGrey(0.722f, 0.753f, 0.784f, 1.f);    // #B8C0C8, ~9.7:1
    const LLColor4 kMonRed(1.f, 0.353f, 0.310f, 1.f);        // #FF5A4F, ~6:1 (the REC light and word)
    const LLColor4 kMonAmber(1.f, 0.761f, 0.200f, 1.f);      // #FFC233, ~11:1
    const LLColor4 kMonGreen(0.298f, 0.765f, 0.541f, 1.f);   // #4CC38A, ~8:1
    const LLColor4 kChipRed(0.776f, 0.157f, 0.157f, 1.f);    // #C62828 behind white text, ~5.6:1
    const LLColor4 kRecordRed(0.878f, 0.204f, 0.169f, 1.f);  // the Record button's dot
    // Level meter zones, our own choice in the broadcast way of leaving headroom: green up to -12 dBFS, amber to -3,
    // red above (where the sound is close to clipping). The scale shows -60 to 0 dBFS.
    constexpr F32 kMeterFloorDb = -60.f, kAmberDb = -12.f, kRedDb = -3.f;
    constexpr F32 kMeterFallDbPerSec = 24.f;   // how fast the bar falls back
    constexpr F64 kPeakHoldSecs = 1.5;         // how long the peak tick stays

    std::string timecode(F64 secs)
    {
        const S32 s = (S32)std::max(0.0, secs);
        return llformat("%02d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    }

    std::string sizeText(U64 bytes)
    {
        if (bytes >= 1024ull * 1024 * 1024) return llformat("%.1f GB", (F64)bytes / (1024.0 * 1024.0 * 1024.0));
        return llformat("%.0f MB", (F64)bytes / (1024.0 * 1024.0));
    }

    std::string durationWords(F64 secs)
    {
        const S64 m = (S64)(secs / 60.0);
        if (m < 1) return "less than a minute";
        if (m < 60) return std::to_string(m) + (m == 1 ? " minute" : " minutes");
        const S64 h = m / 60, mm = m % 60;
        std::string out = std::to_string(h) + (h == 1 ? " hour" : " hours");
        if (mm) out += " " + std::to_string(mm) + (mm == 1 ? " minute" : " minutes");
        return out;
    }

    // a combo's chosen value as a number; its items carry the setting's own numbers (floater_wolf_recorder.xml)
    void bindNumberCombo(LLComboBox* combo, const char* setting)
    {
        combo->setValue(LLSD(std::to_string(gSavedSettings.getS32(setting))));
        combo->setCommitCallback([setting](LLUICtrl* ctrl, const LLSD&) {
            gSavedSettings.setS32(setting, atoi(ctrl->getValue().asString().c_str()));
        });
    }

    const LLFontGL* namedFont(const char* name, const char* size, U8 style, const LLFontGL* fallback)
    {
        const LLFontGL* f = LLFontGL::getFont(LLFontDescriptor(name, size, style));   // fonts.xml (every fonts_*.xml has them)
        return f ? f : fallback;
    }

    void circle(F32 x, F32 y, F32 r, const LLColor4& c)
    {
        gGL.color4fv(c.mV);
        gl_circle_2d(x, y, r, 48, true);
    }

    // The timecode as a camera shows it: each digit centred in a cell as wide as the widest digit (the figures never
    // shift as they change), the colons at their own narrower width.
    void drawTimecode(const LLFontGL* f, const std::string& text, S32 x, S32 y_top, const LLColor4& color)
    {
        S32 cell = 0;
        for (char d = '0'; d <= '9'; ++d) cell = std::max(cell, f->getWidth(std::string(1, d)));
        F32 pen = (F32)x;
        for (char ch : text)
        {
            const std::string one(1, ch);
            const bool digit = ch >= '0' && ch <= '9';
            const S32 w = digit ? cell : f->getWidth(one) + 2;
            f->renderUTF8(one, 0, pen + (F32)w / 2.f, (F32)y_top, color, LLFontGL::HCENTER, LLFontGL::TOP, LLFontGL::NORMAL,
                          LLFontGL::NO_SHADOW);
            pen += (F32)w;
        }
    }

    // a rounded label ("Microphone on") on the monitor; returns its width
    S32 drawChip(S32 x, S32 y_mid, const std::string& text, const LLColor4& bg, const LLColor4& fg)
    {
        const LLFontGL* f = LLFontGL::getFontSansSerifSmallBold();
        const S32 w = f->getWidth(text) + 12, h = (S32)f->getLineHeight() + 6;
        gl_rect_2d(x, y_mid + h / 2, x + w, y_mid - h / 2, bg, true);
        f->renderUTF8(text, 0, (F32)(x + 6), (F32)y_mid, fg, LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::NORMAL,
                      LLFontGL::NO_SHADOW);
        return w;
    }
}

void WolfRecorderTransportButton::draw()
{
    const LLRect r = getLocalRect();
    const bool enabled = isInEnabledChain();
    bool pressed = false;
    if (hasMouseCapture())
    {
        S32 mx = 0, my = 0;
        LLUI::getInstance()->getMousePositionLocal(this, &mx, &my);
        pressed = pointInView(mx, my);
    }
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    // hover and press: a light wash over the whole target (all of it is clickable, word included)
    if (enabled && (pressed || mNeedsHighlight))
    {
        gl_rect_2d(r, LLColor4(1.f, 1.f, 1.f, pressed ? 0.16f : 0.08f), true);
    }
    if (hasFocus())   // the keyboard focus, as every other control shows it
    {
        gl_rect_2d(r.mLeft + 1, r.mTop - 1, r.mRight - 1, r.mBottom + 1, LLUIColorTable::instance().getColor("FocusColor"), false);
    }

    // the round button: a light rim, a dark face, the sign on it
    const F32 d = (F32)(r.getHeight() - 8);
    const F32 cx = (F32)r.mLeft + 4.f + d / 2.f, cy = (F32)r.getHeight() / 2.f;
    const F32 a = enabled ? 1.f : 0.4f;
    circle(cx, cy, d / 2.f, LLColor4(0.90f, 0.91f, 0.92f, a));
    circle(cx, cy, d / 2.f - 3.f, pressed ? LLColor4(0.22f, 0.24f, 0.27f, a) : LLColor4(0.13f, 0.15f, 0.18f, a));
    const LLColor4 red = enabled ? kRecordRed : LLColor4(0.55f, 0.55f, 0.55f, a);
    switch (mIcon)
    {
    case ICON_RECORD:
    case ICON_RESUME:
        circle(cx, cy, d * 0.27f, red);
        break;
    case ICON_STOP:
    {
        const S32 h = (S32)(d * 0.19f);
        gl_rect_2d((S32)cx - h, (S32)cy + h, (S32)cx + h, (S32)cy - h, red, true);
        break;
    }
    case ICON_PAUSE:
    {
        const S32 bh = (S32)(d * 0.21f), bw = std::max(3, (S32)(d * 0.07f)), gap = std::max(3, (S32)(d * 0.06f));
        const LLColor4 bar(0.95f, 0.95f, 0.95f, a);
        gl_rect_2d((S32)cx - gap - bw, (S32)cy + bh, (S32)cx - gap, (S32)cy - bh, bar, true);
        gl_rect_2d((S32)cx + gap, (S32)cy + bh, (S32)cx + gap + bw, (S32)cy - bh, bar, true);
        break;
    }
    }

    // the word beside it, in the skin's own label colour (readable in light and dark skins)
    const LLFontGL* font = getFont() ? getFont() : LLFontGL::getFontSansSerifBig();
    const LLColor4 text = LLUIColorTable::instance().getColor(enabled ? "LabelTextColor" : "LabelDisabledColor");
    font->renderUTF8(getCurrentLabel().getString(), 0, cx + d / 2.f + 12.f, cy, text, LLFontGL::LEFT, LLFontGL::VCENTER,
                     LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
}

WolfFloaterRecorder::WolfFloaterRecorder(const LLSD& key) : LLFloater(key) {}

WolfFloaterRecorder::~WolfFloaterRecorder() = default;

bool WolfFloaterRecorder::postBuild()
{
    mRecordBtn = getChild<LLButton>("record_btn");
    mPauseBtn = getChild<LLButton>("pause_btn");
    mCodecBtn = getChild<LLButton>("codec_btn");
    mPlayBtn = getChild<LLButton>("play_btn");
    mSavedFolderBtn = getChild<LLButton>("saved_folder_btn");
    mStatus = getChild<LLTextBox>("status");
    mMessage = getChild<LLTextBox>("message");
    mCodecNote = getChild<LLTextBox>("codec_note");
    mFolderText = getChild<LLTextBox>("folder_text");
    mWorldNote = getChild<LLTextBox>("world_note");
    mVoiceNote = getChild<LLTextBox>("voice_note");
    mSpaceNote = getChild<LLTextBox>("space_note");
    mSize = getChild<LLComboBox>("size_combo");
    mFps = getChild<LLComboBox>("fps_combo");
    mLimitCombo = getChild<LLComboBox>("limit_combo");
    mMonitor = getChild<LLView>("monitor");
    mScroll = getChild<LLScrollContainer>("settings_scroll");
    mSettingsPanel = getChild<LLView>("settings_panel");
    mMeterViews[WolfRecorder::SOURCE_WORLD] = getChild<LLView>("world_meter");
    mMeterViews[WolfRecorder::SOURCE_VOICE] = getChild<LLView>("voice_meter");
    mMeterViews[WolfRecorder::SOURCE_MIC] = getChild<LLView>("mic_meter");

    mRecordBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRecord(); });
    mPauseBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPause(); });
    getChild<LLButton>("open_folder_btn")->setCommitCallback([](LLUICtrl*, const LLSD&) { WolfRecorder::openFolder(); });
    mSavedFolderBtn->setCommitCallback([](LLUICtrl*, const LLSD&) { WolfRecorder::openFolder(); });
    mPlayBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) {
        std::error_code ec;
        if (mSavedFile.empty() || !std::filesystem::exists(fsyspath(mSavedFile), ec))
        {
            mStartError = getString("play_missing");
            return;
        }
        if (gViewerWindow && gViewerWindow->getWindow()) gViewerWindow->getWindow()->openFile(mSavedFile);
    });
    getChild<LLButton>("change_folder_btn")->setCommitCallback([](LLUICtrl*, const LLSD&) {
        (new LLDirPickerThread(&WolfFloaterRecorder::onFolderPicked, WolfRecorder::folder()))->getFile();
    });
    mCodecBtn->setCommitCallback([](LLUICtrl*, const LLSD&) { WolfVRStream::downloadCodec(); });
    bindNumberCombo(mSize, "WolfRecorderSize");
    bindNumberCombo(mFps, "WolfRecorderFps");
    bindNumberCombo(mLimitCombo, "WolfRecorderMaxMinutes");
    // a changed setting shows at once (setCommitCallback adds a slot: the combos keep bindNumberCombo's, connected first)
    for (const char* name : { "world_check", "voice_check", "mic_check", "show_ui_check", "size_combo", "fps_combo" })
    {
        getChild<LLUICtrl>(name)->setCommitCallback([this](LLUICtrl*, const LLSD&) { mNextStatic = mNextSpace = 0.0; });
    }
    return true;
}

void WolfFloaterRecorder::onOpen(const LLSD& key)
{
    mNextStatic = mNextSpace = 0.0;
    refresh();
}

bool WolfFloaterRecorder::handleToolTip(S32 x, S32 y, MASK mask)
{
    // while the whole window is recorded, no tooltip of this window may end up in the video
    if (WolfRecorder::wantsWholeWindow()) return true;
    return LLFloater::handleToolTip(x, y, mask);
}

// static
void WolfFloaterRecorder::onFolderPicked(const std::vector<std::string>& dirs, std::string proposed)
{
    if (dirs.empty() || dirs[0].empty()) return;
    gSavedSettings.setString("WolfRecorderFolder", dirs[0]);
    if (WolfFloaterRecorder* f = LLFloaterReg::findTypedInstance<WolfFloaterRecorder>("wolf_recorder"))
    {
        f->mNextStatic = f->mNextSpace = 0.0;
    }
}

void WolfFloaterRecorder::onRecord()
{
    mStartError.clear();
    if (WolfRecorder::isRecording())
    {
        WolfRecorder::stop();
    }
    else
    {
        std::string error;
        if (!WolfRecorder::start(error)) mStartError = error;
        else if (WolfRecorder::wantsWholeWindow()) LLToolTipMgr::instance().hideToolTips();   // e.g. Record's own tooltip
    }
    mNextSpace = 0.0;
    refresh();
}

void WolfFloaterRecorder::onPause()
{
    if (WolfRecorder::isPaused()) WolfRecorder::resume();
    else WolfRecorder::pause();
    refresh();
}

void WolfFloaterRecorder::draw()
{
    // a whole-window recording: this window is drawn after the picture copy instead (never in the video)
    if (WolfRecorder::holdBackFromVideo(this)) return;
    if (WolfRecorder::wantsWholeWindow())
    {
        // no tooltip of this window in the video: one already showing when the recording started (the mouse resting
        // on a button) goes; new ones are not made (handleToolTip). The tooltip view is part of the recorded UI.
        LLToolTipMgr& tips = LLToolTipMgr::instance();
        S32 mx = 0, my = 0;
        LLUI::getInstance()->getMousePositionLocal(this, &mx, &my);
        if (tips.toolTipVisible() && (pointInView(mx, my) || tips.getMouseNearRect().overlaps(calcScreenRect())))
        {
            tips.hideToolTips();
        }
    }
    refresh();
    followKeyboardFocus();
    updatePreview();
    updateMeters();
    LLFloater::draw();
    if (isMinimized()) return;
    drawMonitor();
    // the meters scroll with the settings: drawn clipped to the scroll container's visible part, so one scrolled out
    // of sight never paints over the buttons (LLLocalClipRect: in this window's coordinates, the current UI origin)
    LLRect visible;
    if (!mScroll->localRectToOtherView(mScroll->getContentWindowRect(), &visible, this)) return;
    LLLocalClipRect clip(visible);
    for (int i = 0; i < WolfRecorder::SOURCE_COUNT; ++i)
    {
        LLRect r;
        if (mMeterViews[i]->localRectToOtherView(mMeterViews[i]->getLocalRect(), &r, this) && r.overlaps(visible))
        {
            drawMeter(i, r);
        }
    }
}

void WolfFloaterRecorder::followKeyboardFocus()
{
    // a control reached with the keyboard (Tab) is scrolled into view: LLScrollContainer does not follow the focus
    // itself; scrollToShowRect takes the rect in the scrolled panel's coordinates (llscrollcontainer.cpp)
    LLFocusableElement* focus = gFocusMgr.getKeyboardFocus();
    if (focus == mLastFocus) return;
    mLastFocus = focus;
    LLView* v = dynamic_cast<LLView*>(focus);
    if (!v || !v->hasAncestor(mSettingsPanel)) return;
    LLRect r;
    if (v->localRectToOtherView(v->getLocalRect(), &r, mSettingsPanel))
    {
        mScroll->scrollToShowRect(r);
        LL_DEBUGS("WolfRecorder") << "keyboard focus on " << v->getName() << ": scrolled to show it" << LL_ENDL;
    }
}

void WolfFloaterRecorder::updatePreview()
{
    if (!WolfRecorder::previewPicture(mPreviewSerial, mPreviewRGB)) return;
    constexpr U32 W = WolfRecorder::PREVIEW_WIDTH, H = WolfRecorder::PREVIEW_HEIGHT;
    if (mPreviewRGB.size() != (size_t)W * H * 3) return;
    // GL textures here are a power of two on each side (llimagegl.cpp LLImageGL::setSize, checkSize): the 16:9 picture
    // goes in the bottom rows of a W x W texture (its rows are bottom first), drawn with a UV rect
    // covering only those rows (drawMonitor)
    static_assert((W & (W - 1)) == 0 && H <= W, "the preview texture is W x W, a power of two");
    LLPointer<LLImageRaw> raw = new LLImageRaw((U16)W, (U16)W, 3);
    std::memset(raw->getData(), 0, (size_t)W * W * 3);
    std::memcpy(raw->getData(), mPreviewRGB.data(), mPreviewRGB.size());
    // a small upload (192 KB, four times a second) - the picture itself came from the encoder thread's own copy
    if (mPreviewTex.isNull() || !mPreviewTex->getGLTexture() || !mPreviewTex->getGLTexture()->getTexName())
    {
        mPreviewTex = LLViewerTextureManager::getLocalTexture(raw.get(), false);
    }
    else
    {
        mPreviewTex->getGLTexture()->setSubImage(raw, 0, 0, (S32)W, (S32)W);
    }
}

void WolfFloaterRecorder::updateMeters()
{
    F32 peaks[WolfRecorder::SOURCE_COUNT] = {};
    WolfRecorder::levels(peaks);
    const F32 dt = llclamp(mMeterTimer.getElapsedTimeAndResetF32(), 0.f, 0.5f);
    const F64 now = LLFrameTimer::getElapsedSeconds();
    for (int i = 0; i < WolfRecorder::SOURCE_COUNT; ++i)
    {
        const F32 db = peaks[i] > 1e-5f ? 20.f * std::log10(peaks[i]) : -90.f;   // dBFS of the loudest sample
        mMeterDb[i] = std::max(db, mMeterDb[i] - kMeterFallDbPerSec * dt);
        if (db >= mHoldDb[i])
        {
            mHoldDb[i] = db;
            mHoldUntil[i] = now + kPeakHoldSecs;
        }
        else if (now > mHoldUntil[i])
        {
            mHoldDb[i] = std::max(db, mHoldDb[i] - kMeterFallDbPerSec * dt);
        }
    }
}

void WolfFloaterRecorder::drawMeter(int source, const LLRect& rect)
{
    // the bar (12 px, centred) with room for the word "Loud" on its right
    const LLFontGL* font = LLFontGL::getFontSansSerifSmallBold();
    const S32 word_w = font->getWidth(getString("meter_loud")) + 6;
    const S32 left = rect.mLeft, right = rect.mRight - word_w, mid = rect.mBottom + rect.getHeight() / 2;
    const S32 top = mid + 6, bottom = mid - 6, width = right - left;
    if (width < 20) return;
    auto xAt = [&](F32 db) { return left + (S32)((F32)width * llclamp((db - kMeterFloorDb) / -kMeterFloorDb, 0.f, 1.f)); };
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gl_rect_2d(left, top, right, bottom, LLColor4(0.12f, 0.13f, 0.15f, 1.f), true);
    const F32 level = mMeterDb[source];
    if (level > kMeterFloorDb)
    {
        const S32 x = xAt(level), xa = xAt(kAmberDb), xr = xAt(kRedDb);
        gl_rect_2d(left, top, std::min(x, xa), bottom, LLColor4(0.180f, 0.722f, 0.447f, 1.f), true);   // green
        if (x > xa) gl_rect_2d(xa, top, std::min(x, xr), bottom, LLColor4(0.949f, 0.718f, 0.020f, 1.f), true);   // amber
        if (x > xr) gl_rect_2d(xr, top, x, bottom, LLColor4(0.898f, 0.200f, 0.165f, 1.f), true);   // red
    }
    // the zone marks, so the scale reads even before a recording
    const LLColor4 mark(1.f, 1.f, 1.f, 0.35f);
    for (F32 db : { kAmberDb, kRedDb })
    {
        const S32 x = xAt(db);
        gl_rect_2d(x, top, x + 1, bottom, mark, true);
    }
    if (mHoldDb[source] > kMeterFloorDb)
    {
        const S32 x = xAt(mHoldDb[source]);
        gl_rect_2d(x - 1, top, x + 1, bottom, LLColor4(1.f, 1.f, 1.f, 0.9f), true);
    }
    gl_rect_2d(left, top, right, bottom, LLColor4(0.f, 0.f, 0.f, 0.6f), false);
    // too loud in words as well as in red (WCAG 1.4.1)
    if (mHoldDb[source] >= kRedDb)
    {
        font->renderUTF8(getString("meter_loud"), 0, (F32)(right + 6), (F32)mid,
                         LLUIColorTable::instance().getColor("AlertCautionTextColor"), LLFontGL::LEFT, LLFontGL::VCENTER,
                         LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
    }
}

void WolfFloaterRecorder::drawMonitor()
{
    LLRect m;
    if (!mMonitor->localRectToOtherView(mMonitor->getLocalRect(), &m, this)) return;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gl_rect_2d(m, kMonBg, true);
    gl_rect_2d(m, kMonEdge, false);

    // the preview: 16:9, at most a little over half the monitor's width
    S32 ph = m.getHeight() - 16, pw = ph * 16 / 9;
    if (pw > m.getWidth() * 55 / 100)
    {
        pw = m.getWidth() * 55 / 100;
        ph = pw * 9 / 16;
    }
    const LLRect p(m.mLeft + 8, m.mTop - 8, m.mLeft + 8 + pw, m.mTop - 8 - ph);
    gl_rect_2d(p, LLColor4::black, true);
    if (mPreviewTex.notNull() && mPreviewTex->getGLTexture() && mPreviewTex->getGLTexture()->getTexName())
    {
        // the picture's rows only (updatePreview: PREVIEW_HEIGHT of the texture's PREVIEW_WIDTH rows, from the bottom)
        const F32 v_top = (F32)WolfRecorder::PREVIEW_HEIGHT / (F32)WolfRecorder::PREVIEW_WIDTH;
        gl_draw_scaled_image(p.mLeft, p.mBottom, pw, ph, mPreviewTex, LLColor4::white, LLRectf(0.f, v_top, 1.f, 0.f));
    }
    else
    {
        const LLFontGL* f = LLFontGL::getFontSansSerif();
        f->renderUTF8(getString("preview_empty_1"), 0, (F32)(p.mLeft + pw / 2), (F32)(p.mBottom + ph / 2 + 2), kMonGrey,
                      LLFontGL::HCENTER, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
        f->renderUTF8(getString("preview_empty_2"), 0, (F32)(p.mLeft + pw / 2), (F32)(p.mBottom + ph / 2 - 2), kMonGrey,
                      LLFontGL::HCENTER, LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
    }
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gl_rect_2d(p, kMonEdge, false);

    // the right-hand side: the state with its light, the timecode, the detail line, the limit, the sound chips
    const S32 x = p.mRight + 16;
    S32 y = m.mTop - 12;
    const LLFontGL* state_font = namedFont("B612", "Avionics7", LLFontGL::BOLD, LLFontGL::getFontSansSerifBold());
    const LLFontGL* time_font = namedFont("B612", "Avionics11", LLFontGL::BOLD, LLFontGL::getFontSansSerifHuge());
    const S32 sh = (S32)state_font->getLineHeight();
    const F32 lcx = (F32)x + 7.f, lcy = (F32)(y - sh / 2);
    const auto state = (WolfRecorder::State)mState;
    std::string word;
    LLColor4 word_color = kMonGrey;
    if (state == WolfRecorder::State::RECORDING && mPaused)
    {
        const S32 bx = x + 1;
        gl_rect_2d(bx, (S32)lcy + 7, bx + 4, (S32)lcy - 7, kMonAmber, true);
        gl_rect_2d(bx + 8, (S32)lcy + 7, bx + 12, (S32)lcy - 7, kMonAmber, true);
        word = getString("state_paused");
        word_color = kMonAmber;
    }
    else if (state == WolfRecorder::State::RECORDING || state == WolfRecorder::State::STARTING)
    {
        // the REC light, pulsing once a second
        const F32 t = (F32)std::fmod(mSeconds, 1.0);
        const F32 pulse = 0.55f + 0.45f * (0.5f + 0.5f * std::cos(t * F_TWO_PI));
        circle(lcx, lcy, 9.f, LLColor4(kMonRed.mV[VRED], kMonRed.mV[VGREEN], kMonRed.mV[VBLUE], 0.25f * pulse));
        circle(lcx, lcy, 6.5f, LLColor4(kMonRed.mV[VRED], kMonRed.mV[VGREEN], kMonRed.mV[VBLUE], pulse));
        word = getString(state == WolfRecorder::State::STARTING ? "state_starting" : "state_rec");
        word_color = kMonRed;
    }
    else if (state == WolfRecorder::State::FINISHING)
    {
        circle(lcx, lcy, 6.5f, kMonGrey);
        word = getString("state_saving");
    }
    else
    {
        circle(lcx, lcy, 6.5f, mSaved ? kMonGreen : kMonGrey);
        circle(lcx, lcy, 4.5f, mSaved ? kMonGreen : kMonBg);
        word = getString(mSaved ? "state_saved" : "state_ready");
        word_color = mSaved ? kMonGreen : kMonGrey;
    }
    state_font->renderUTF8(word, 0, (F32)(x + 22), lcy, word_color, LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::NORMAL,
                           LLFontGL::NO_SHADOW);
    y -= sh + 6;

    const bool live = state == WolfRecorder::State::RECORDING || state == WolfRecorder::State::STARTING ||
                      state == WolfRecorder::State::FINISHING;
    // after Stop the saved video's length stays beside SAVED until the next Record (the same Report::length as the
    // "Saved: ..." line under the buttons), not 00:00:00
    drawTimecode(time_font, timecode(live ? mSeconds : (mSaved ? mSavedLength : 0.0)), x, y, kMonText);
    y -= (S32)time_font->getLineHeight() + 6;

    const LLFontGL* f = LLFontGL::getFontSansSerif();
    f->renderUTF8(mDetail, 0, (F32)x, (F32)y, kMonGrey, LLFontGL::LEFT, LLFontGL::TOP, LLFontGL::NORMAL,
                  LLFontGL::NO_SHADOW, S32_MAX, m.mRight - x - 8);
    y -= (S32)f->getLineHeight() + 4;

    if (mLimitSecs > 0.0 && (state == WolfRecorder::State::RECORDING || state == WolfRecorder::State::STARTING))
    {
        LLStringUtil::format_map_t args;
        args["[END]"] = timecode(mLimitSecs);
        const bool near_end = mLimitSecs - mSeconds <= 30.0;
        f->renderUTF8(getString("monitor_stops_at", args), 0, (F32)x, (F32)y, near_end ? kMonAmber : kMonGrey,
                      LLFontGL::LEFT, LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::NO_SHADOW, S32_MAX, m.mRight - x - 8);
    }

    // the sound chips, at the bottom: what is being heard into the video right now
    if (state == WolfRecorder::State::RECORDING && !mPaused)
    {
        S32 cx = x;
        const S32 cy = m.mBottom + 16;
        if (mMicOn) cx += drawChip(cx, cy, getString("chip_mic"), kChipRed, LLColor4::white) + 6;
        if (mVoiceOn) drawChip(cx, cy, getString("chip_voice"), kMonEdge, kMonText);
    }
}

void WolfFloaterRecorder::refreshStatic()
{
    // world sound: available this session, or why not
    std::string world_note;
    if (WolfRecorder::worldSoundNeedsRestart()) world_note = getString("world_restart");
    else if (gSavedSettings.getBOOL("WolfRecorderSoundWorld") && !WolfRecorder::worldSoundAvailable())
    {
#if LL_OPENAL
        world_note = getString("world_unavailable");
#else
        world_note = getString("world_fmod");
#endif
    }
    mWorldNote->setText(world_note);
    // the privacy note only matters while voice is ticked
    mVoiceNote->setVisible(gSavedSettings.getBOOL("WolfRecorderSoundVoice"));

    const std::string folder = WolfRecorder::folder();
    mFolderText->setText(folder);
    mFolderText->setToolTip(folder);
    mNextStatic = LLFrameTimer::getElapsedSeconds() + 2.0;
}

void WolfFloaterRecorder::refreshSpace(const WolfRecorder::Report& st)
{
    // the free space on the folder's disk, and how long a recording can run before the disk guard stops it
    // (WolfRecorder::DISK_STOP_BYTES): at the current rate while recording, else at this size
    std::error_code ec;
    fsyspath probe(WolfRecorder::folder());
    while (!probe.empty() && !std::filesystem::exists(probe, ec) && probe.has_parent_path() && probe.parent_path() != probe)
    {
        probe = fsyspath(probe.parent_path());   // a folder not made yet: its nearest existing parent's disk
    }
    const std::filesystem::space_info si = std::filesystem::space(probe, ec);
    mNextSpace = LLFrameTimer::getElapsedSeconds() + 2.0;
    if (ec)
    {
        mSpaceNote->setText(LLStringUtil::null);
        return;
    }
    const bool recording = st.state == WolfRecorder::State::RECORDING;
    F64 bytes_per_sec = 0.0;
    if (recording && st.seconds >= 3.0 && st.bytes > 0)
    {
        bytes_per_sec = (F64)st.bytes / st.seconds;
    }
    else
    {
        // the bit rates WolfRecorder::start chooses (6 or 10 Mbit/s, half as much again at 60 a second) + the sound
        const bool hd = gSavedSettings.getS32("WolfRecorderSize") >= 1080;
        const bool smooth = gSavedSettings.getS32("WolfRecorderFps") >= 60;
        bytes_per_sec = ((hd ? 10000.0 : 6000.0) * (smooth ? 1.5 : 1.0) + 160.0) * 1000.0 / 8.0;
    }
    const U64 avail = (U64)si.available;
    const U64 usable = avail > WolfRecorder::DISK_STOP_BYTES ? avail - WolfRecorder::DISK_STOP_BYTES : 0;
    LLStringUtil::format_map_t args;
    args["[FREE]"] = sizeText(avail);
    args["[TIME]"] = durationWords((F64)usable / std::max(1.0, bytes_per_sec));
    mSpaceNote->setText(getString(recording ? "space_recording" : "space_free", args));
}

void WolfFloaterRecorder::refresh()
{
    if (!mRecordBtn) return;
    const WolfRecorder::Report st = WolfRecorder::status();
    const bool busy = st.state != WolfRecorder::State::IDLE;
    const bool recording = st.state == WolfRecorder::State::STARTING || st.state == WolfRecorder::State::RECORDING;
    mState = (S32)st.state;
    mPaused = st.paused;
    mSeconds = st.seconds;
    mLimitSecs = st.limit_seconds;
    mMicOn = st.mic;
    mVoiceOn = st.voice;
    mSaved = st.state == WolfRecorder::State::IDLE && st.saved && !st.file.empty();
    mSavedLength = mSaved ? st.length : 0.0;
    if (mSaved && st.file != mSavedFile)
    {
        // the file's real size (Report::bytes counts the samples only; the file also holds ~4 MB of reserved headers,
        // wolfrec_mp4.h bytesWritten), read once for each saved file
        mSavedFile = st.file;
        std::error_code ec;
        const std::uintmax_t size = std::filesystem::file_size(fsyspath(mSavedFile), ec);
        mSavedBytes = ec ? st.bytes : (U64)size;
    }

    // the transport buttons
    WolfRecorderTransportButton* rec = dynamic_cast<WolfRecorderTransportButton*>(mRecordBtn);
    WolfRecorderTransportButton* pau = dynamic_cast<WolfRecorderTransportButton*>(mPauseBtn);
    if (rec) rec->setIcon(recording ? WolfRecorderTransportButton::ICON_STOP : WolfRecorderTransportButton::ICON_RECORD);
    mRecordBtn->setLabel(getString(recording ? "stop_label" : (busy ? "saving_label" : "record_label")));
    mRecordBtn->setToolTip(getString(recording ? "stop_tip" : "record_tip"));
    mRecordBtn->setEnabled(st.state != WolfRecorder::State::FINISHING);
    if (pau) pau->setIcon(st.paused ? WolfRecorderTransportButton::ICON_RESUME : WolfRecorderTransportButton::ICON_PAUSE);
    mPauseBtn->setLabel(getString(st.paused ? "resume_label" : "pause_label"));
    mPauseBtn->setToolTip(getString(st.paused ? "resume_tip" : "pause_tip"));
    mPauseBtn->setEnabled(st.state == WolfRecorder::State::RECORDING);

    // the line under the timecode
    if (st.state == WolfRecorder::State::RECORDING && st.paused)
    {
        mDetail = getString("detail_paused");
    }
    else if (st.state == WolfRecorder::State::RECORDING)
    {
        LLStringUtil::format_map_t args, dargs;
        args["[FPS]"] = llformat("%.0f", st.fps);
        dargs["[N]"] = llformat("%u", st.dropped);
        args["[DROPPED]"] = st.dropped ? getString("dropped", dargs) : std::string();
        mDetail = getString("detail_recording", args);
    }
    else if (st.state == WolfRecorder::State::IDLE)
    {
        const bool hd = gSavedSettings.getS32("WolfRecorderSize") >= 1080;
        LLStringUtil::format_map_t args;
        args["[SIZE]"] = hd ? "1920 x 1080" : "1280 x 720";
        args["[FPS]"] = gSavedSettings.getS32("WolfRecorderFps") >= 60 ? "60" : "30";
        mDetail = getString("detail_idle", args);
    }
    else
    {
        mDetail.clear();
    }

    // the plain-words status under the buttons
    std::string status;
    switch (st.state)
    {
    case WolfRecorder::State::IDLE:
        if (mSaved)
        {
            LLStringUtil::format_map_t args;
            args["[FILE]"] = gDirUtilp->getBaseFileName(st.file);
            args["[SIZE]"] = sizeText(mSavedBytes);
            args["[LENGTH]"] = timecode(st.length);
            status = getString("saved_as", args);
        }
        else status = getString("ready");
        break;
    case WolfRecorder::State::STARTING:
        status = getString("starting");
        break;
    case WolfRecorder::State::RECORDING:
        if (st.paused)
        {
            status = getString("paused");
        }
        else if (st.limit_seconds > 0.0 && st.limit_seconds - st.seconds <= 30.0)
        {
            LLStringUtil::format_map_t args;
            args["[END]"] = timecode(st.limit_seconds);
            status = getString("near_end", args);
        }
        else
        {
            status = getString("recording");
        }
        break;
    case WolfRecorder::State::FINISHING:
        status = getString("finishing");
        break;
    }
    mStatus->setText(status);

    // a problem (stays until the next press) or a note; "Problem:" in words as well as colour (WCAG 1.4.1)
    const std::string error = !mStartError.empty() ? mStartError : st.error;
    if (!error.empty())
    {
        mMessage->setText("Problem: " + error);
        mMessage->setColor(LLUIColorTable::instance().getColor("AlertCautionTextColor"));
    }
    else
    {
        mMessage->setText(st.notice == "Saved." ? std::string() : st.notice);
        mMessage->setColor(LLUIColorTable::instance().getColor("LabelTextColor"));
    }

    // the Cisco codec: offered only when there is something to download and the encoders found nothing else
    const int codec = WolfVRStream::codecState();
    const bool offer_codec = (codec == 0 || codec == 2) && !error.empty() && error.find("video encoder") != std::string::npos;
    mCodecBtn->setVisible(offer_codec);
    mCodecBtn->setEnabled(codec == 0);
    mCodecNote->setVisible(offer_codec);   // Cisco's binary licence, condition 3: the attribution where it is controlled
    if (offer_codec || codec == 2)
    {
        const std::string codec_msg = WolfVRStream::codecMessage();   // the download's own plain-words answer
        if (!codec_msg.empty()) mMessage->setText(mMessage->getText() + "\n" + codec_msg);
    }
    // after a save: play it, or show it in its folder
    mPlayBtn->setVisible(mSaved && !offer_codec);
    mSavedFolderBtn->setVisible(mSaved && !offer_codec);

    // settings are fixed for a recording (the levels stay live); a list left open when Record was pressed closes, so
    // no list of this window is ever in the video (combo lists are popups of the whole UI, not of this window)
    for (const char* name : { "size_combo", "fps_combo", "show_ui_check", "world_check", "voice_check", "mic_check",
                              "change_folder_btn", "limit_combo" })
    {
        getChild<LLUICtrl>(name)->setEnabled(!busy);
    }
    if (busy)
    {
        for (LLComboBox* combo : { mSize, mFps, mLimitCombo }) combo->hideList();
    }
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (!busy && now >= mNextStatic) refreshStatic();
    if (now >= mNextSpace) refreshSpace(st);
}
