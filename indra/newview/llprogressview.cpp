/**
 * @file llprogressview.cpp
 * @brief LLProgressView class implementation
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llprogressview.h"

#include "indra_constants.h"
#include "llmath.h"
#include "llgl.h"
#include "llrender.h"
#include "llui.h"
#include "llfontgl.h"
#include "lltimer.h"
#include "lltextbox.h"
#include "llglheaders.h"

#include "llagent.h"
#include "llbutton.h"
#include "llcallbacklist.h"
#include "llfocusmgr.h"
#include "llnotifications.h"
#include "llprogressbar.h"
#include "llstartup.h"
#include "llviewercontrol.h"
#include "llviewertexturelist.h"
#include "llviewerwindow.h"
#include "llappviewer.h"
#include "llweb.h"
#include "lluictrlfactory.h"
#include "llversioninfo.h"   // <WolfViewer 2026-09-28/> release name in the heading
// <FS:Ansariel> [FS Login Panel]
//#include "llpanellogin.h"
#include "fspanellogin.h"
// </FS:Ansariel> [FS Login Panel]

LLProgressView* LLProgressView::sInstance = NULL;
LLProgressViewMini* LLProgressViewMini::sInstance = NULL;

S32 gStartImageWidth = 1;
S32 gStartImageHeight = 1;
const F32 FADE_TO_WORLD_TIME = 1.0f;

static LLPanelInjector<LLProgressView> r("progress_view");
static LLPanelInjector<LLProgressViewMini> r_mini("progress_view_mini");

LLProgressViewMini::LLProgressViewMini()
{
    sInstance = this;
}

bool LLProgressViewMini::postBuild()
{
    mCancelBtn=getChild<LLButton>("cancel_btn");
    mCancelBtn->setClickedCallback(LLProgressViewMini::onCancelButtonClicked, nullptr);

    mProgressBar=getChild<LLProgressBar>("progress_bar_mini");
    mProgressText=getChild<LLTextBox>("progress_text");

    return true;
}

void LLProgressViewMini::setPercent(const F32 percent)
{
    mProgressBar->setValue(percent);

    // hide ourselves when 100% is reached. This is necessary because the login code
    // expects the fullscreen panel to hide itself when login is completed
    if (percent == 100.0f)
        setVisible(false);
}

void LLProgressViewMini::setText(const std::string& text)
{
    mProgressText->setValue(text);
}

void LLProgressViewMini::setCancelButtonVisible(bool b, const std::string& label)
{
    mCancelBtn->setVisible(b);
    mCancelBtn->setEnabled(b);
    mCancelBtn->setLabelSelected(label);
    mCancelBtn->setLabelUnselected(label);
}

// static
void LLProgressViewMini::onCancelButtonClicked(void* dummy)
{
    // code reuse is good, even if we have an unnecessary hiding of the full screen tp window there
    // we might have to reconsider this in case we change setVisible(false) to fade(false) in there. -Zi
    LLProgressView::onCancelButtonClicked(dummy);
    sInstance->mCancelBtn->setEnabled(false);
    sInstance->setVisible(false);
}

// XUI: Translate
LLProgressView::LLProgressView()
:   LLPanel(),
    mPercentDone( 0.f ),
    mMediaCtrl( NULL ),
    mMouseDownInActiveArea( false ),
    mUpdateEvents("LLProgressView"),
    mFadeToWorldTimer(),
    mFadeFromLoginTimer(),
    mStartupComplete(false)
{
    mUpdateEvents.listen("self", boost::bind(&LLProgressView::handleUpdate, this, _1));
    mFadeToWorldTimer.stop();
    mFadeFromLoginTimer.stop();
}

bool LLProgressView::postBuild()
{
    mProgressBar = getChild<LLProgressBar>("login_progress_bar");

    mLogosLabel = getChild<LLTextBox>("logos_lbl");

    mProgressText = getChild<LLTextBox>("progress_text");
    mMessageText = getChild<LLTextBox>("message_text");
    mMessageTextRectInitial = mMessageText->getRect(); // auto resizes, save initial size

    // media control that is used to play intro video
    mMediaCtrl = getChild<LLMediaCtrl>("login_media_panel");
    mMediaCtrl->setVisible( false );        // hidden initially
    mMediaCtrl->addObserver( this );        // watch events

    LLViewerMedia::getInstance()->setOnlyAudibleMediaTextureID(mMediaCtrl->getTextureID());

    mCancelBtn = getChild<LLButton>("cancel_btn");
    mCancelBtn->setClickedCallback(  LLProgressView::onCancelButtonClicked, NULL );

    mLayoutPanel4 = getChild<LLView>("panel4");
    mLayoutPanel4RectInitial = mLayoutPanel4->getRect();

    mLayoutMOTD = getChild<LLView>("panel_motd");
    mLayoutMOTDRectInitial = mLayoutMOTD->getRect();

    // <WolfViewer 2026-09-28> The release in the loading box's heading (Paul: "I diddnt see the
    // release name when i logged in", with a screenshot of this box reading "WolfViewer_x64").
    // Worded as the window title (llappviewer.cpp); a build with no release name keeps the
    // app name. " · AVX2" on the AVX2 build — About's SIMD flag. getSecondLifeTitle() itself is
    // left alone: it also names the viewer to the crash logger.
    //getChild<LLTextBox>("title_text")->setText(LLStringExplicit(LLAppViewer::instance()->getSecondLifeTitle()));
    {
        const std::string release_name = LLVersionInfo::getInstance()->getReleaseName();
        std::string heading = release_name.empty() ? LLAppViewer::instance()->getSecondLifeTitle()
                                                   : std::string("WolfViewer \xE2\x80\x94 ") + release_name;   // U+2014 em dash
#ifdef USE_AVX2_OPTIMIZATION
        heading += " \xC2\xB7 AVX2";   // U+00B7 middle dot
#endif
        getChild<LLTextBox>("title_text")->setText(LLStringExplicit(heading));
    }
    // </WolfViewer>

    getChild<LLTextBox>("message_text")->setClickedCallback(onClickMessage, this);

    // hidden initially, until we need it
    setVisible(false);

    LLNotifications::instance().getChannel("AlertModal")->connectChanged(boost::bind(&LLProgressView::onAlertModal, this, _1));

    sInstance = this;
    return true;
}


LLProgressView::~LLProgressView()
{
    // Just in case something went wrong, make sure we deregister our idle callback.
    gIdleCallbacks.deleteFunction(onIdle, this);

    gFocusMgr.releaseFocusIfNeeded( this );

    sInstance = NULL;
}

bool LLProgressView::handleHover(S32 x, S32 y, MASK mask)
{
    if( childrenHandleHover( x, y, mask ) == NULL )
    {
        gViewerWindow->setCursor(UI_CURSOR_WAIT);
    }
    return true;
}


bool LLProgressView::handleKeyHere(KEY key, MASK mask)
{
    // Suck up all keystokes except CTRL-Q.
    if( ('Q' == key) && (MASK_CONTROL == mask) )
    {
        LLAppViewer::instance()->userQuit();
    }
    return true;
}

void LLProgressView::revealIntroPanel()
{
    // if user hasn't yet seen intro video
    std::string intro_url = gSavedSettings.getString("PostFirstLoginIntroURL");
    if ( intro_url.length() > 0 &&
            gSavedSettings.getBOOL("BrowserJavascriptEnabled") &&
            !gSavedSettings.getBOOL("PostFirstLoginIntroViewed"))
    {
        // hide the progress bar
        getChild<LLView>("stack1")->setVisible(false);

        // navigate to intro URL and reveal widget
        mMediaCtrl->navigateTo( intro_url );
        mMediaCtrl->setVisible( true );


        // flag as having seen the new user post login intro
        gSavedSettings.setBOOL("PostFirstLoginIntroViewed", true );

        mMediaCtrl->setFocus(true);
    }

    mFadeFromLoginTimer.start();
    gIdleCallbacks.addFunction(onIdle, this);
}

void LLProgressView::setStartupComplete()
{
    mStartupComplete = true;

    // if we are not showing a video, fade into world
    if (!mMediaCtrl->getVisible())
    {
        mFadeFromLoginTimer.stop();
        mFadeToWorldTimer.start();
    }

    // <FS:NickyD> FIRE-3063; Enable Audio for all media sources again. They got disabled during postBuild(), but as we never reach LLProgressView::draw
    // if the progress is disabled, we would never get media audio back.
    LLViewerMedia::getInstance()->setOnlyAudibleMediaTextureID(LLUUID::null);
}

void LLProgressView::setVisible(bool visible)
{
    if (!visible && mFadeFromLoginTimer.getStarted())
    {
        mFadeFromLoginTimer.stop();
    }
    // hiding progress view
    if (getVisible() && !visible)
    {
        LLPanel::setVisible(false);
    }
    // showing progress view
    else if (visible && (!getVisible() || mFadeToWorldTimer.getStarted()))
    {
        setFocus(true);
        mFadeToWorldTimer.stop();
        LLPanel::setVisible(true);
    }
}

// <FS:Zi> Fade teleport screens
void LLProgressView::fade(bool in)
{
    if(in)
    {
        mFadeFromLoginTimer.start();
        mFadeToWorldTimer.stop();
        setVisible(true);
    }
    else
    {
        mFadeFromLoginTimer.stop();
        mFadeToWorldTimer.start();
        // set visibility will be done in the draw() method after fade
    }
}
// </FS:Zi> Fade teleport screens

void LLProgressView::drawStartTexture(F32 alpha)
{
    gGL.pushMatrix();
    if (gStartTexture)
    {
        LLGLSUIDefault gls_ui;
        gGL.getTexUnit(0)->bind(gStartTexture.get());
        gGL.color4f(1.f, 1.f, 1.f, alpha);
        F32 image_aspect = (F32)gStartImageWidth / (F32)gStartImageHeight;
        S32 width = getRect().getWidth();
        S32 height = getRect().getHeight();
        F32 view_aspect = (F32)width / (F32)height;
        // stretch image to maintain aspect ratio
        if (image_aspect > view_aspect)
        {
            gGL.translatef(-0.5f * (image_aspect / view_aspect - 1.f) * width, 0.f, 0.f);
            gGL.scalef(image_aspect / view_aspect, 1.f, 1.f);
        }
        else
        {
            gGL.translatef(0.f, -0.5f * (view_aspect / image_aspect - 1.f) * height, 0.f);
            gGL.scalef(1.f, view_aspect / image_aspect, 1.f);
        }
        gl_rect_2d_simple_tex( getRect().getWidth(), getRect().getHeight() );
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    }
    else
    {
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.color4f(0.f, 0.f, 0.f, alpha);      // ## Zi: Fade teleport screens
        gl_rect_2d(getRect());
    }
    gGL.popMatrix();
}

void LLProgressView::drawLogos(F32 alpha)
{
    if (mLogosList.empty())
    {
        return;
    }

    // logos are tied to label,
    // due to potential resizes we have to figure offsets out on draw or resize
    S32 offset_x, offset_y;
    mLogosLabel->localPointToScreen(0, 0, &offset_x, &offset_y);
    std::vector<TextureData>::const_iterator iter = mLogosList.begin();
    std::vector<TextureData>::const_iterator end = mLogosList.end();
    for (; iter != end; iter++)
    {
        gl_draw_scaled_image_with_border(iter->mDrawRect.mLeft + offset_x,
                             iter->mDrawRect.mBottom + offset_y,
                             iter->mDrawRect.getWidth(),
                             iter->mDrawRect.getHeight(),
                             iter->mTexturep.get(),
                             UI_VERTEX_COLOR % alpha,
                             false,
                             iter->mClipRect,
                             iter->mOffsetRect);
    }
}

// We need these images very early, so we have to force-load them, otherwise they might not load
// in time. Null when the file is missing or does not decode. (Was the body of loadLogo.)
static LLPointer<LLViewerTexture> load_local_image(const std::string& path, const U8 image_codec)
{
    if (!gDirUtilp->fileExists(path))
    {
        return nullptr;
    }

    LLPointer<LLImageFormatted> start_image_frmted = LLImageFormatted::createFromType(image_codec);
    if (!start_image_frmted->load(path))
    {
        LL_WARNS("AppInit") << "Image load failed: " << path << LL_ENDL;
        return nullptr;
    }

    LLPointer<LLImageRaw> raw = new LLImageRaw;
    if (!start_image_frmted->decode(raw, 0.0f))
    {
        LL_WARNS("AppInit") << "Image decode failed " << path << LL_ENDL;
        return nullptr;
    }
    // HACK: getLocalTexture allows only power of two dimentions
    raw->expandToPowerOfTwo();

    return LLViewerTextureManager::getLocalTexture(raw.get(), false);
}

// <WolfViewer 2026-09-28> Paul: "that loading bar on login is dog boring ... could we have a
// dancing wolf". Frames: skins/default/textures/wolf_dance (README.txt: Wolf by Quaternius, CC0,
// dance keyed and rendered for WolfViewer). Loaded like the logos — force-loaded, because this
// screen is up before the texture pipeline is — and released with them.
namespace
{
    constexpr S32 DANCE_FRAMES = 32;
    constexpr F32 DANCE_FPS = 24.f;
    constexpr S32 DANCE_MAX_PX = 150;   // the wolf's square, at most
    constexpr S32 DANCE_MIN_PX = 60;    // smaller than this and it is left out
    constexpr S32 DANCE_GAP_PX = 8;     // from the bar below and the heading beside
}

void LLProgressView::initDancingWolf()
{
    mDanceFrames.clear();
    if (!gSavedSettings.getBOOL("WolfLoadingDancingWolf"))
    {
        return;
    }
    const std::string dir = gDirUtilp->getExpandedFilename(LL_PATH_DEFAULT_SKIN, "textures", "wolf_dance");
    for (S32 i = 0; i < DANCE_FRAMES; ++i)
    {
        LLPointer<LLViewerTexture> frame = load_local_image(dir + gDirUtilp->getDirDelimiter() + llformat("wolf_dance_%02d.png", i), IMG_CODEC_PNG);
        if (frame.isNull())
        {
            // A missing frame would stutter the loop: no wolf rather than a broken one.
            LL_WARNS("AppInit") << "Dancing wolf frame " << i << " missing in " << dir << "; not shown" << LL_ENDL;
            mDanceFrames.clear();
            return;
        }
        mDanceFrames.push_back(frame);
    }
    mDanceTimer.reset();
    mDanceTimer.start();
}

void LLProgressView::drawDancingWolf(F32 alpha)
{
    if (mDanceFrames.empty() || !mProgressBar || !mLayoutPanel4)
    {
        return;
    }
    // Placed from the live widgets each frame, as drawLogos does, because the box resizes (the MOTD
    // grows it): above the progress bar, right-aligned with it, below the top of the box, and never
    // over the heading.
    LLRect bar, box;
    mProgressBar->localRectToScreen(mProgressBar->getLocalRect(), &bar);
    mLayoutPanel4->localRectToScreen(mLayoutPanel4->getLocalRect(), &box);
    S32 size = llmin(DANCE_MAX_PX, box.mTop - bar.mTop - 2 * DANCE_GAP_PX);
    S32 left = bar.mRight - size;
    if (LLTextBox* title = findChild<LLTextBox>("title_text"))
    {
        LLRect title_rect;
        title->localRectToScreen(title->getLocalRect(), &title_rect);
        const S32 text_right = title_rect.mLeft + (S32)title->getFont()->getWidthF32(title->getWText().c_str()) + DANCE_GAP_PX;
        if (left < text_right)
        {
            size -= text_right - left;
            left = text_right;
        }
    }
    if (size < DANCE_MIN_PX)
    {
        return;
    }
    const S32 frame = (S32)(mDanceTimer.getElapsedTimeF32() * DANCE_FPS) % DANCE_FRAMES;
    gl_draw_scaled_image(left, bar.mTop + DANCE_GAP_PX, size, size, mDanceFrames[frame].get(), UI_VERTEX_COLOR % alpha);
}
// </WolfViewer>

void LLProgressView::draw()
{
    static LLTimer timer;

    if (mFadeFromLoginTimer.getStarted())
    {
        F32 alpha = clamp_rescale(mFadeFromLoginTimer.getElapsedTimeF32(), 0.f, FADE_TO_WORLD_TIME, 0.f, 1.f);
        LLViewDrawContext context(alpha);

        if (!mMediaCtrl->getVisible())
        {
            drawStartTexture(alpha);
        }

        LLPanel::draw();
        drawLogos(alpha);
        drawDancingWolf(alpha);   // <WolfViewer 2026-09-28/>
        return;
    }

    // handle fade out to world view when we're asked to
    if (mFadeToWorldTimer.getStarted())
    {
        // draw fading panel
        F32 alpha = clamp_rescale(mFadeToWorldTimer.getElapsedTimeF32(), 0.f, FADE_TO_WORLD_TIME, 1.f, 0.f);
        LLViewDrawContext context(alpha);

        drawStartTexture(alpha);
        LLPanel::draw();
        drawLogos(alpha);
        drawDancingWolf(alpha);   // <WolfViewer 2026-09-28/>

        // faded out completely - remove panel and reveal world
        if (mFadeToWorldTimer.getElapsedTimeF32() > FADE_TO_WORLD_TIME )
        {
            mFadeToWorldTimer.stop();

            LLViewerMedia::getInstance()->setOnlyAudibleMediaTextureID(LLUUID::null);

            // Fade is complete, release focus
            gFocusMgr.releaseFocusIfNeeded( this );

            // turn off panel that hosts intro so we see the world
            setVisible(false);

            // stop observing events since we no longer care
            mMediaCtrl->remObserver( this );

            // hide the intro
            mMediaCtrl->setVisible( false );

            // navigate away from intro page to something innocuous since 'unload' is broken right now
            //mMediaCtrl->navigateTo( "about:blank" );

            // FIXME: this causes a crash that i haven't been able to fix
            mMediaCtrl->unloadMediaSource();

            releaseTextures();
        }
        return;
    }

    drawStartTexture(1.0f);
    // draw children
    LLPanel::draw();
    drawLogos(1.0f);
    drawDancingWolf(1.0f);   // <WolfViewer 2026-09-28/>
}

void LLProgressView::setText(const std::string& text)
{
    mProgressText->setValue(text);
}

void LLProgressView::setPercent(const F32 percent)
{
    mProgressBar->setValue(percent);
}

void LLProgressView::setMessage(const std::string& msg)
{
    mMessage = msg;
    mMessageText->setValue(mMessage);
    S32 height = mMessageText->getTextPixelHeight();
    S32 delta  = height - mMessageTextRectInitial.getHeight();
    if (delta > 0)
    {
        mLayoutPanel4->reshape(mLayoutPanel4RectInitial.getWidth(), mLayoutPanel4RectInitial.getHeight() + delta);
        mLayoutMOTD->reshape(mLayoutMOTDRectInitial.getWidth(), mLayoutMOTDRectInitial.getHeight() + delta);
    }
    else
    {
        mLayoutPanel4->reshape(mLayoutPanel4RectInitial.getWidth(), mLayoutPanel4RectInitial.getHeight());
        mLayoutMOTD->reshape(mLayoutMOTDRectInitial.getWidth(), mLayoutMOTDRectInitial.getHeight());
    }
}

void LLProgressView::loadLogo(const std::string &path,
                              const U8 image_codec,
                              const LLRect &pos_rect,
                              const LLRectf &clip_rect,
                              const LLRectf &offset_rect)
{
    // <WolfViewer 2026-09-28> The force-load moved to load_local_image (above drawDancingWolf)
    // so the dancing wolf's frames load the same way; behaviour unchanged.
    LLPointer<LLViewerTexture> texture = load_local_image(path, image_codec);
    if (texture.isNull())
    {
        return;
    }

    TextureData data;
    data.mTexturep = texture;
    data.mDrawRect = pos_rect;
    data.mClipRect = clip_rect;
    data.mOffsetRect = offset_rect;
    mLogosList.push_back(data);
}

void LLProgressView::initLogos()
{
    mLogosList.clear();

    const U8 image_codec = IMG_CODEC_PNG;
    const LLRectf default_clip(0.f, 1.f, 1.f, 0.f);
    const S32 default_height = 28;
    const S32 default_pad = 15;

    S32 icon_width, icon_height;

    // We don't know final screen rect yet, so we can't precalculate position fully
    S32 texture_start_x = (S32)mLogosLabel->getFont()->getWidthF32(mLogosLabel->getWText().c_str()) + default_pad;
    S32 texture_start_y = -7;

    // Normally we would just preload these textures from textures.xml,
    // and display them via icon control, but they are only needed on
    // startup and preloaded/UI ones stay forever
    // (and this code was done already so simply reused it)
    std::string temp_str = gDirUtilp->getExpandedFilename(LL_PATH_DEFAULT_SKIN, "textures", "3p_icons");

    temp_str += gDirUtilp->getDirDelimiter();

#ifdef LL_FMODSTUDIO
    // original image size is 264x96, it is on longer side but
    // with no internal paddings so it gets additional padding
    icon_width = 77;
    icon_height = 21;
    S32 pad_fmod_y = 4;
    texture_start_x++;
    loadLogo(temp_str + "fmod_logo.png",
        image_codec,
        LLRect(texture_start_x, texture_start_y + pad_fmod_y + icon_height, texture_start_x + icon_width, texture_start_y + pad_fmod_y),
        default_clip,
        default_clip);

    texture_start_x += icon_width + default_pad + 1;
#endif //LL_FMODSTUDIO
#ifdef LL_HAVOK
    // original image size is 342x113, central element is on a larger side
    // plus internal padding, so it gets slightly more height than desired 32
    icon_width = 88;
    icon_height = 29;
    S32 pad_havok_y = -1;
    loadLogo(temp_str + "havok_logo.png",
        image_codec,
        LLRect(texture_start_x, texture_start_y + pad_havok_y + icon_height, texture_start_x + icon_width, texture_start_y + pad_havok_y),
        default_clip,
        default_clip);

    texture_start_x += icon_width + default_pad;
#endif //LL_HAVOK

    // <WolfViewer 2026-09-12> The Vivox logo used to be drawn here, and the label beside it read
    // "[APP_NAME] uses". We do not use Vivox: voice on Wolf Territories is WolfVoice, our own
    // spatial WebRTC backend. The attribution was untrue, and it was the only logo on that line
    // once FMOD and Havok are compiled out (ReleaseOS), so the label now carries the whole
    // sentence — "[APP_NAME] uses WolfVoice" — rather than trailing off into an empty row.
    // No logo is drawn in its place: there is no WolfVoice artwork, and inventing some would be
    // worse than saying it plainly.
    (void)icon_width;
    (void)icon_height;
}

void LLProgressView::initStartTexture(S32 location_id, bool is_in_production)
{
    if (gStartTexture.notNull())
    {
        gStartTexture = NULL;
        LL_INFOS("AppInit") << "re-initializing start screen" << LL_ENDL;
    }

    LL_DEBUGS("AppInit") << "Loading startup bitmap..." << LL_ENDL;

    U8 image_codec = IMG_CODEC_PNG;
    std::string temp_str = gDirUtilp->getLindenUserDir() + gDirUtilp->getDirDelimiter();

    if ((S32)START_LOCATION_ID_LAST == location_id)
    {
        temp_str += LLStartUp::getScreenLastFilename();
    }
    else
    {
        std::string path = temp_str + LLStartUp::getScreenHomeFilename();

        if (!gDirUtilp->fileExists(path) && is_in_production)
        {
            // Fallback to old file, can be removed later
            // Home image only sets when user changes home, so it will take time for users to switch to pngs
            temp_str += "screen_home.bmp";
            image_codec = IMG_CODEC_BMP;
        }
        else
        {
            temp_str = path;
        }
    }

    LLPointer<LLImageFormatted> start_image_frmted = LLImageFormatted::createFromType(image_codec);

    // Turn off start screen to get around the occasional readback
    // driver bug
    if (!gSavedSettings.getBOOL("UseStartScreen"))
    {
        LL_INFOS("AppInit") << "Bitmap load disabled" << LL_ENDL;
        return;
    }
    else if (!start_image_frmted->load(temp_str))
    {
        LL_WARNS("AppInit") << "Bitmap load failed" << LL_ENDL;
        gStartTexture = NULL;
    }
    else
    {
        gStartImageWidth = start_image_frmted->getWidth();
        gStartImageHeight = start_image_frmted->getHeight();

        LLPointer<LLImageRaw> raw = new LLImageRaw;
        if (!start_image_frmted->decode(raw, 0.0f))
        {
            LL_WARNS("AppInit") << "Bitmap decode failed" << LL_ENDL;
            gStartTexture = NULL;
        }
        else
        {
            // HACK: getLocalTexture allows only power of two dimentions
            raw->expandToPowerOfTwo();
            gStartTexture = LLViewerTextureManager::getLocalTexture(raw.get(), false);
        }
    }

    if (gStartTexture.isNull())
    {
        gStartTexture = LLViewerTexture::sBlackImagep;
        gStartImageWidth = gStartTexture->getWidth();
        gStartImageHeight = gStartTexture->getHeight();
    }
}

void LLProgressView::initTextures(S32 location_id, bool is_in_production)
{
    initStartTexture(location_id, is_in_production);
    initLogos();
    initDancingWolf();   // <WolfViewer 2026-09-28/>

    childSetVisible("panel_icons", !mLogosList.empty());
    childSetVisible("panel_top_spacer", mLogosList.empty());
}

void LLProgressView::releaseTextures()
{
    gStartTexture = NULL;
    mLogosList.clear();
    mDanceFrames.clear();   // <WolfViewer 2026-09-28/> login only, like the logos

    childSetVisible("panel_top_spacer", true);
    childSetVisible("panel_icons", false);
}

void LLProgressView::setCancelButtonVisible(bool b, const std::string& label)
{
    mCancelBtn->setVisible(b);
    mCancelBtn->setEnabled(b);
    mCancelBtn->setLabelSelected(label);
    mCancelBtn->setLabelUnselected(label);
}

// static
void LLProgressView::onCancelButtonClicked(void*)
{
    // Quitting viewer here should happen only when "Quit" button is pressed while starting up.
    // Check for startup state is used here instead of teleport state to avoid quitting when
    // cancel is pressed while teleporting inside region (EXT-4911)
    if (LLStartUp::getStartupState() < STATE_STARTED)
    {
        LL_INFOS() << "User requesting quit during login" << LL_ENDL;
        LLAppViewer::instance()->requestQuit();
    }
    else
    {
        gAgent.teleportCancel();
        sInstance->mCancelBtn->setEnabled(false);
        sInstance->setVisible(false);
    }
}

// static
void LLProgressView::onClickMessage(void* data)
{
    LLProgressView* viewp = (LLProgressView*)data;
    if ( viewp != NULL && ! viewp->mMessage.empty() )
    {
        std::string url_to_open( "" );

        size_t start_pos;
        start_pos = viewp->mMessage.find( "https://" );
        if (start_pos == std::string::npos)
            start_pos = viewp->mMessage.find( "http://" );
        if (start_pos == std::string::npos)
            start_pos = viewp->mMessage.find( "ftp://" );

        if ( start_pos != std::string::npos )
        {
            size_t end_pos = viewp->mMessage.find_first_of( " \n\r\t", start_pos );
            if ( end_pos != std::string::npos )
                url_to_open = viewp->mMessage.substr( start_pos, end_pos - start_pos );
            else
                url_to_open = viewp->mMessage.substr( start_pos );

            LLWeb::loadURLExternal( url_to_open );
        }
    }
}

bool LLProgressView::handleUpdate(const LLSD& event_data)
{
    LLSD message = event_data.get("message");
    LLSD desc = event_data.get("desc");
    LLSD percent = event_data.get("percent");

    if(message.isDefined())
    {
        setMessage(message.asString());
    }

    if(desc.isDefined())
    {
        setText(desc.asString());
    }

    if(percent.isDefined())
    {
        setPercent((F32)percent.asReal());
    }
    return false;
}

bool LLProgressView::onAlertModal(const LLSD& notify)
{
    // if the progress view is visible, it will obscure the notification window
    // in this case, we want to auto-accept WebLaunchExternalTarget notifications
    if (isInVisibleChain() && notify["sigtype"].asString() == "add")
    {
        LLNotificationPtr notifyp = LLNotifications::instance().find(notify["id"].asUUID());
        if (notifyp && notifyp->getName() == "WebLaunchExternalTarget")
        {
            notifyp->respondWithDefault();
        }
    }
    return false;
}

void LLProgressView::handleMediaEvent(LLPluginClassMedia* self, EMediaEvent event)
{
    // the intro web content calls javascript::window.close() when it's done
    if( event == MEDIA_EVENT_CLOSE_REQUEST )
    {
        if (mStartupComplete)
        {
            //make sure other timer has stopped
            mFadeFromLoginTimer.stop();
            mFadeToWorldTimer.start();
        }
        else
        {
            // hide the media ctrl and wait for startup to be completed before fading to world
            mMediaCtrl->setVisible(false);
            if (mMediaCtrl->getMediaPlugin())
            {
                mMediaCtrl->getMediaPlugin()->stop();
            }

            // show the progress bar
            getChild<LLView>("stack1")->setVisible(true);
        }
    }
}


// static
void LLProgressView::onIdle(void* user_data)
{
    LLProgressView* self = (LLProgressView*) user_data;

    // Close login panel on mFadeToWorldTimer expiration.
    if (self->mFadeFromLoginTimer.getStarted() &&
        self->mFadeFromLoginTimer.getElapsedTimeF32() > FADE_TO_WORLD_TIME)
    {
        self->mFadeFromLoginTimer.stop();
        // <FS:Ansariel> [FS Login Panel]
        //LLPanelLogin::closePanel();
        FSPanelLogin::closePanel();
        // </FS:Ansariel> [FS Login Panel]

        // Nothing to do anymore.
        gIdleCallbacks.deleteFunction(onIdle, user_data);
    }
}
