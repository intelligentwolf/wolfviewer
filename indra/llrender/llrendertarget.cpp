/**
 * @file llrendertarget.cpp
 * @brief LLRenderTarget implementation
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
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

#include "linden_common.h"

#include "llrendertarget.h"
#include <algorithm>   // [G8 2026-11-11] std::find in release()
#include "llrender.h"
#include "llgl.h"

LLRenderTarget* LLRenderTarget::sBoundTarget = NULL;
U32 LLRenderTarget::sBytesAllocated = 0;

void check_framebuffer_status()
{
    if (gDebugGL)
    {
        GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        switch (status)
        {
        case GL_FRAMEBUFFER_COMPLETE:
            break;
        default:
            LL_WARNS() << "check_framebuffer_status failed -- " << std::hex << status << LL_ENDL;
            ll_fail("check_framebuffer_status failed");
            break;
        }
    }
}

bool LLRenderTarget::sUseFBO = false;
U32 LLRenderTarget::sCurFBO = 0;
LLRenderTarget::WolfVkProvider* LLRenderTarget::sWolfVkProvider = nullptr;   // [VULKAN G-BUFFER 2026-11-08/]
void (*LLRenderTarget::sWolfVkBeforeTargetChange)() = nullptr;   // [VULKAN G-BUFFER S2 2026-11-26/]
LLRenderTarget::WolfVkNativeOwner* LLRenderTarget::sWolfVkNativeOwner = nullptr;   // [NATIVE VULKAN 2026-11-26/]

// [VK S6 REGISTRY 2026-11-26] the targets that name a provider of their own or hold textures from one, so a provider
// that goes away can be taken out of all of them (llrendertarget.h)
namespace
{
    std::vector<LLRenderTarget*>& wolfVkTargets()
    {
        static std::vector<LLRenderTarget*> targets;
        return targets;
    }
    void wolfVkTrack(LLRenderTarget* target)
    {
        auto& targets = wolfVkTargets();
        if (std::find(targets.begin(), targets.end(), target) == targets.end()) targets.push_back(target);
    }
    void wolfVkUntrack(LLRenderTarget* target)
    {
        auto& targets = wolfVkTargets();
        targets.erase(std::remove(targets.begin(), targets.end(), target), targets.end());
    }
}

//static
void LLRenderTarget::wolfVkRegister(LLRenderTarget& target, WolfVkProvider* provider)
{
    target.mWolfVkProvider = provider;
    if (provider || target.mWolfVkProvidedBy) wolfVkTrack(&target);
    else wolfVkUntrack(&target);
}

//static
void LLRenderTarget::wolfVkUnregisterProvider(WolfVkProvider* provider)
{
    if (!provider) return;
    auto& targets = wolfVkTargets();
    for (auto it = targets.begin(); it != targets.end(); )
    {
        LLRenderTarget* target = *it;
        if (target->mWolfVkProvider == provider) target->mWolfVkProvider = nullptr;
        if (target->mWolfVkProvidedBy == provider) target->mWolfVkProvidedBy = nullptr;
        if (!target->mWolfVkProvider && !target->mWolfVkProvidedBy) it = targets.erase(it);
        else ++it;
    }
    if (sWolfVkProvider == provider) sWolfVkProvider = nullptr;
}
bool LLRenderTarget::sWolfDepth32F = false;   // [AMD PERF 2026-11-08/] testing, llrendertarget.h


extern S32 gGLViewport[4];

U32 LLRenderTarget::sCurResX = 0;
U32 LLRenderTarget::sCurResY = 0;

LLRenderTarget::LLRenderTarget() :
    mResX(0),
    mResY(0),
    mFBO(0),
    mDepth(0),
    mUseDepth(false),
    mUsage(LLTexUnit::TT_TEXTURE)
{
}

LLRenderTarget::~LLRenderTarget()
{
    release();
    wolfVkUntrack(this);   // [VK S6 REGISTRY 2026-11-26] a target with a provider of its own stays listed until it goes
}

void LLRenderTarget::resize(U32 resx, U32 resy)
{
    //for accounting, get the number of pixels added/subtracted
    S32 pix_diff = (resx*resy)-(mResX*mResY);

    mResX = resx;
    mResY = resy;

    llassert(mInternalFormat.size() == mTex.size());

    for (U32 i = 0; i < mTex.size(); ++i)
    { //resize color attachments
        gGL.getTexUnit(0)->bindManual(mUsage, mTex[i]);
        LLImageGL::setManualImage(LLTexUnit::getInternalType(mUsage), 0, mInternalFormat[i], mResX, mResY, GL_RGBA, GL_UNSIGNED_BYTE, NULL, false);
        sBytesAllocated += pix_diff*4;
    }

    if (mDepth)
    {
        gGL.getTexUnit(0)->bindManual(mUsage, mDepth);
        U32 internal_type = LLTexUnit::getInternalType(mUsage);
        LLImageGL::setManualImage(internal_type, 0, sWolfDepth32F ? GL_DEPTH_COMPONENT32F : GL_DEPTH_COMPONENT24, mResX, mResY, GL_DEPTH_COMPONENT, sWolfDepth32F ? GL_FLOAT : GL_UNSIGNED_INT, NULL, false);   // [AMD PERF 2026-11-08/] sWolfDepth32F: testing

        sBytesAllocated += pix_diff*4;
    }
}


bool LLRenderTarget::allocate(U32 resx, U32 resy, U32 color_fmt, bool depth, LLTexUnit::eTextureType usage, LLTexUnit::eTextureMipGeneration generateMipMaps)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    llassert(usage == LLTexUnit::TT_TEXTURE);
    llassert(!isBoundInStack());

    if(mResX == resx && mResY == resy && mUsage == usage && depth == mUseDepth && mGenerateMipMaps == generateMipMaps)
    {
        return true;
    }
    resx = llmin(resx, (U32) gGLManager.mGLMaxTextureSize);
    resy = llmin(resy, (U32) gGLManager.mGLMaxTextureSize);

    release();

    mResX = resx;
    mResY = resy;

    mUsage = usage;
    mUseDepth = depth;

    mGenerateMipMaps = generateMipMaps;

    if (mGenerateMipMaps != LLTexUnit::TMG_NONE) {
        // Calculate the number of mip levels based upon resolution that we should have.
        mMipLevels = 1 + (U32)floor(log10((float)llmax(mResX, mResY)) / log10(2.0));
    }

    if (depth)
    {
        if (!allocateDepth())
        {
            LL_WARNS() << "Failed to allocate depth buffer for render target." << LL_ENDL;
            return false;
        }
    }

    if (gGLManager.mIsNativeVulkan)
    {
        static U32 sNativeFBO = 1;
        mFBO = sNativeFBO++;
        bool ok = addColorAttachment(color_fmt);
        if (ok && sWolfVkNativeOwner) sWolfVkNativeOwner->onTargetChanged(*this);
        return ok;
    }

    glGenFramebuffers(1, (GLuint *) &mFBO);

    if (mDepth)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, mFBO);

        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, LLTexUnit::getInternalType(mUsage), mDepth, 0);

        glBindFramebuffer(GL_FRAMEBUFFER, sCurFBO);
    }

    return addColorAttachment(color_fmt);
}

void LLRenderTarget::setColorAttachment(LLImageGL* img, LLGLuint use_name)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    llassert(img != nullptr); // img must not be null
    llassert(sUseFBO); // FBO support must be enabled
    llassert(mDepth == 0); // depth buffers not supported with this mode
    llassert(mTex.empty()); // mTex must be empty with this mode (binding target should be done via LLImageGL)
    llassert(!isBoundInStack());

    if (gGLManager.mIsNativeVulkan)
    {
        if (mFBO == 0) { static U32 sNativeFBO2 = 0x10000; mFBO = sNativeFBO2++; }
        mResX = img->getWidth();
        mResY = img->getHeight();
        mUsage = img->getTarget();
        if (use_name == 0) use_name = img->getTexName();
        mTex.push_back(use_name);
        if (sWolfVkNativeOwner) sWolfVkNativeOwner->onTargetChanged(*this);
        return;
    }
    if (mFBO == 0)
    {
        glGenFramebuffers(1, (GLuint*)&mFBO);
    }

    mResX = img->getWidth();
    mResY = img->getHeight();
    mUsage = img->getTarget();

    if (use_name == 0)
    {
        use_name = img->getTexName();
    }

    mTex.push_back(use_name);

    glBindFramebuffer(GL_FRAMEBUFFER, mFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            LLTexUnit::getInternalType(mUsage), use_name, 0);
        stop_glerror();

    check_framebuffer_status();

    glBindFramebuffer(GL_FRAMEBUFFER, sCurFBO);
}

void LLRenderTarget::releaseColorAttachment()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    llassert(!isBoundInStack());
    llassert(mTex.size() == 1); //cannot use releaseColorAttachment with LLRenderTarget managed color targets
    llassert(mFBO != 0);  // mFBO must be valid

    if (gGLManager.mIsNativeVulkan)
    {
        mTex.clear();
        if (sWolfVkNativeOwner) sWolfVkNativeOwner->onTargetChanged(*this);
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, mFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, LLTexUnit::getInternalType(mUsage), 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, sCurFBO);

    mTex.clear();
}

bool LLRenderTarget::addColorAttachment(U32 color_fmt)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    llassert(!isBoundInStack());

    if (color_fmt == 0)
    {
        return true;
    }

    U32 offset = static_cast<U32>(mTex.size());

    if( offset >= 4 )
    {
        LL_WARNS() << "Too many color attachments" << LL_ENDL;
        llassert( offset < 4 );
        return false;
    }
    if( offset > 0 && (mFBO == 0) )
    {
        llassert(  mFBO != 0 );
        return false;
    }

    U32 tex = 0;
    bool provided = false;
    WolfVkProvider* wolf_provider = wolfVkProvider();   // [VK S6 REGISTRY 2026-11-26] this target's own, or the shared one
    // one provider per target at a time: textures already held from another are not mixed with a new one's
    if (wolf_provider && mWolfVkProvidedBy && mWolfVkProvidedBy != wolf_provider) wolf_provider = nullptr;
    if (wolf_provider && mUsage == LLTexUnit::TT_TEXTURE)
    {
        // [VULKAN G-BUFFER 2026-11-08] a texture over memory the Vulkan renderer exported (llrendertarget.h WolfVkProvider)
        provided = wolf_provider->colorTexture(*this, offset, color_fmt, tex);
        if (provided)
        {
            mWolfVkProvidedNames.push_back(tex);   // [G8 2026-11-11]
            mWolfVkProvidedBy = wolf_provider;
            wolfVkTrack(this);
        }
    }
    if (!provided)
    {
        LLImageGL::generateTextures(1, &tex);
    }
    gGL.getTexUnit(0)->bindManual(mUsage, tex);

    stop_glerror();


    if (!provided)
    {
        clear_glerror();
        if (gGLManager.mIsNativeVulkan && mGenerateMipMaps != LLTexUnit::TMG_NONE && LLImageGL::sWolfVkNativeStore)
        {
            LLImageGL::sWolfVkNativeStore->uploadImage2D(tex, 0, color_fmt, mResX, mResY, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, false, true);
        }
        else
        {
            LLImageGL::setManualImage(LLTexUnit::getInternalType(mUsage), 0, color_fmt, mResX, mResY, GL_RGBA, GL_UNSIGNED_BYTE, NULL, false);
        }
        if (!gGLManager.mIsNativeVulkan && glGetError() != GL_NO_ERROR)
        {
            LL_WARNS() << "Could not allocate color buffer for render target." << LL_ENDL;
            return false;
        }
    }

    sBytesAllocated += mResX*mResY*4;

    stop_glerror();


    if (offset == 0)
    { //use bilinear filtering on single texture render targets that aren't multisampled
        gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
        stop_glerror();
    }
    else
    { //don't filter data attachments
        gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_POINT);
        stop_glerror();
    }

    if (mUsage != LLTexUnit::TT_RECT_TEXTURE)
    {
        gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_MIRROR);
        stop_glerror();
    }
    else
    {
        // ATI doesn't support mirrored repeat for rectangular textures.
        gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
        stop_glerror();
    }

    if (mFBO && !gGLManager.mIsNativeVulkan)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, mFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0+offset,
            LLTexUnit::getInternalType(mUsage), tex, 0);

        check_framebuffer_status();

        glBindFramebuffer(GL_FRAMEBUFFER, sCurFBO);
    }

    mTex.push_back(tex);
    mInternalFormat.push_back(color_fmt);
    if (gGLManager.mIsNativeVulkan && sWolfVkNativeOwner) sWolfVkNativeOwner->onTargetChanged(*this);

    if (gDebugGL && !gGLManager.mIsNativeVulkan)
    { //bind and unbind to validate target
        bindTarget();
        flush();
    }


    return true;
}

bool LLRenderTarget::allocateDepth()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    WolfVkProvider* wolf_provider = wolfVkProvider();   // [VK S6 REGISTRY 2026-11-26]
    if (wolf_provider && mWolfVkProvidedBy && mWolfVkProvidedBy != wolf_provider) wolf_provider = nullptr;
    if (wolf_provider && mUsage == LLTexUnit::TT_TEXTURE)
    {
        // [VULKAN G-BUFFER 2026-11-08] the shared depth (GL_DEPTH_COMPONENT32F over a Vulkan D32_SFLOAT image)
        U32 tex = 0;
        if (wolf_provider->depthTexture(*this, tex))
        {
            mDepth = tex;
            mWolfVkProvidedNames.push_back(tex);   // [G8 2026-11-11]
            mWolfVkProvidedBy = wolf_provider;
            wolfVkTrack(this);
            gGL.getTexUnit(0)->bindManual(mUsage, mDepth);
            gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_POINT);
            sBytesAllocated += mResX*mResY*4;
            return true;
        }
    }
    LLImageGL::generateTextures(1, &mDepth);
    gGL.getTexUnit(0)->bindManual(mUsage, mDepth);

    U32 internal_type = LLTexUnit::getInternalType(mUsage);
    stop_glerror();
    clear_glerror();
    LLImageGL::setManualImage(internal_type, 0, sWolfDepth32F ? GL_DEPTH_COMPONENT32F : GL_DEPTH_COMPONENT24, mResX, mResY, GL_DEPTH_COMPONENT, sWolfDepth32F ? GL_FLOAT : GL_UNSIGNED_INT, NULL, false);   // [AMD PERF 2026-11-08/] sWolfDepth32F: testing
    gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_POINT);

    sBytesAllocated += mResX*mResY*4;

    if (!gGLManager.mIsNativeVulkan && glGetError() != GL_NO_ERROR)
    {
        LL_WARNS() << "Unable to allocate depth buffer for render target." << LL_ENDL;
        return false;
    }

    return true;
}

void LLRenderTarget::shareDepthBuffer(LLRenderTarget& target)
{
    llassert(!isBoundInStack());

    if (!mFBO || !target.mFBO)
    {
        LL_ERRS() << "Cannot share depth buffer between non FBO render targets." << LL_ENDL;
    }

    if (target.mDepth)
    {
        LL_ERRS() << "Attempting to override existing depth buffer.  Detach existing buffer first." << LL_ENDL;
    }

    if (target.mUseDepth)
    {
        LL_ERRS() << "Attempting to override existing shared depth buffer. Detach existing buffer first." << LL_ENDL;
    }

    if (mDepth)
    {
        if (gGLManager.mIsNativeVulkan)
        {
            target.mUseDepth = true;
            if (sWolfVkNativeOwner) sWolfVkNativeOwner->onShareDepth(*this, target);
            return;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, target.mFBO);

        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, LLTexUnit::getInternalType(mUsage), mDepth, 0);

        check_framebuffer_status();

        glBindFramebuffer(GL_FRAMEBUFFER, sCurFBO);

        target.mUseDepth = true;
    }
}

void LLRenderTarget::release()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    llassert(!isBoundInStack());

    // [VULKAN G-BUFFER 2026-11-08] textures from the provider belong to it: it hears of the release and deletes them itself
    // [G8 2026-11-11] only those: every texture the target made itself is deleted here as before
    std::vector<U32> provided_names;
    provided_names.swap(mWolfVkProvidedNames);
    const auto provided = [&provided_names](U32 tex) { return std::find(provided_names.begin(), provided_names.end(), tex) != provided_names.end(); };
    // [VK S6 REGISTRY 2026-11-26] the provider they came from hears of it (null when it has unregistered itself since)
    WolfVkProvider* provided_by = mWolfVkProvidedBy;
    mWolfVkProvidedBy = nullptr;
    if (!mWolfVkProvider) wolfVkUntrack(this);
    // a provider reached through the static pointer is told only while that pointer still names it, as before the
    // registry: WolfVkBridge clears the pointer and deletes the G-buffer provider before the target is released
    if (provided_by && provided_by != mWolfVkProvider && provided_by != sWolfVkProvider) provided_by = nullptr;
    if (!provided_names.empty() && provided_by)
    {
        provided_by->released(*this);
    }

    if (mDepth)
    {
        if (!provided(mDepth)) LLImageGL::deleteTextures(1, &mDepth);

        mDepth = 0;

        sBytesAllocated -= mResX*mResY*4;
    }
    if (gGLManager.mIsNativeVulkan)
    {
        mUseDepth = false;
        for (size_t z = mTex.size(); z > 1; --z)
        {
            sBytesAllocated -= mResX*mResY*4;
            if (!provided(mTex[z - 1])) LLImageGL::deleteTextures(1, &mTex[z - 1]);
        }
        if (mFBO)
        {
            if (mFBO == sCurFBO) sCurFBO = 0;
            if (sWolfVkNativeOwner) sWolfVkNativeOwner->onReleaseTarget(*this);
            mFBO = 0;
        }
    }
    else
    {
        // else if (mFBO)
        if (mFBO)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, mFBO);

            if (mUseDepth)
            { //detach shared depth buffer
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, LLTexUnit::getInternalType(mUsage), 0, 0);
                mUseDepth = false;
            }

            glBindFramebuffer(GL_FRAMEBUFFER, sCurFBO);
        }

        // Detach any extra color buffers (e.g. SRGB spec buffers)
        //
        if (mFBO && (mTex.size() > 1))
        {
            glBindFramebuffer(GL_FRAMEBUFFER, mFBO);
            size_t z;
            for (z = mTex.size() - 1; z >= 1; z--)
            {
                sBytesAllocated -= mResX*mResY*4;
                glFramebufferTexture2D(GL_FRAMEBUFFER, static_cast<GLenum>(GL_COLOR_ATTACHMENT0+z), LLTexUnit::getInternalType(mUsage), 0, 0);
                if (!provided(mTex[z])) LLImageGL::deleteTextures(1, &mTex[z]);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, sCurFBO);
        }

        if (mFBO)
        {
            if (mFBO == sCurFBO)
            {
                sCurFBO = 0;
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
            }

            glDeleteFramebuffers(1, (GLuint *) &mFBO);
            mFBO = 0;
        }
    }

    if (mTex.size() > 0)
    {
        sBytesAllocated -= mResX*mResY*4;
        if (!provided(mTex[0])) LLImageGL::deleteTextures(1, &mTex[0]);
    }

    mTex.clear();
    mInternalFormat.clear();

    mResX = mResY = 0;
}

void LLRenderTarget::bindTarget()
{
    if (sWolfVkBeforeTargetChange) sWolfVkBeforeTargetChange();   // [VULKAN G-BUFFER S2 2026-11-26/] llrendertarget.h
    LL_PROFILE_GPU_ZONE("bindTarget");
    llassert(mFBO);
    llassert(!isBoundInStack());

    if (gGLManager.mIsNativeVulkan)
    {
        sCurFBO = mFBO;
        sCurResX = mResX;
        sCurResY = mResY;
        mPreviousRT = sBoundTarget;
        sBoundTarget = this;
        if (sWolfVkNativeOwner) sWolfVkNativeOwner->onBindTarget(*this);
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, mFBO);
    sCurFBO = mFBO;

    //setup multiple render targets
    GLenum drawbuffers[] = {GL_COLOR_ATTACHMENT0,
                            GL_COLOR_ATTACHMENT1,
                            GL_COLOR_ATTACHMENT2,
                            GL_COLOR_ATTACHMENT3};

    if (mTex.empty())
    { //no color buffer to draw to
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
    }
    else
    {
        glDrawBuffers(static_cast<GLsizei>(mTex.size()), drawbuffers);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
    }
    check_framebuffer_status();

    glViewport(0, 0, mResX, mResY);
    sCurResX = mResX;
    sCurResY = mResY;

    mPreviousRT = sBoundTarget;
    sBoundTarget = this;
}

void LLRenderTarget::clear(U32 mask_in)
{
    if (sWolfVkBeforeTargetChange) sWolfVkBeforeTargetChange();   // [VULKAN G-BUFFER S2 2026-11-26/] llrendertarget.h
    LL_PROFILE_GPU_ZONE("clear");
    llassert(mFBO);
    U32 mask = GL_COLOR_BUFFER_BIT;
    if (mUseDepth)
    {
        mask |= GL_DEPTH_BUFFER_BIT;

    }
    if (gGLManager.mIsNativeVulkan)
    {
        LLGLState::wolfClear(mask & mask_in);
        return;
    }
    if (mFBO)
    {
        check_framebuffer_status();
        stop_glerror();
        glClear(mask & mask_in);
        stop_glerror();
    }
    else
    {
        LLGLEnable scissor(GL_SCISSOR_TEST);
        glScissor(0, 0, mResX, mResY);
        stop_glerror();
        glClear(mask & mask_in);
    }
}

U32 LLRenderTarget::getTexture(U32 attachment) const
{
    if (attachment >= mTex.size())
    {
        LL_WARNS() << "Invalid attachment index " << attachment << " for size " << mTex.size() << LL_ENDL;
        llassert(false);
        return 0;
    }
    return mTex[attachment];
}

U32 LLRenderTarget::getNumTextures() const
{
    return static_cast<U32>(mTex.size());
}

void LLRenderTarget::bindTexture(U32 index, S32 channel, LLTexUnit::eTextureFilterOptions filter_options)
{
    gGL.getTexUnit(channel)->bindManual(mUsage, getTexture(index), filter_options == LLTexUnit::TFO_TRILINEAR || filter_options == LLTexUnit::TFO_ANISOTROPIC);
    gGL.getTexUnit(channel)->setTextureFilteringOption(filter_options);
}

void LLRenderTarget::flush()
{
    if (sWolfVkBeforeTargetChange) sWolfVkBeforeTargetChange();   // [VULKAN G-BUFFER S2 2026-11-26/] llrendertarget.h
    LL_PROFILE_GPU_ZONE("rt flush");
    gGL.flush();
    llassert(mFBO);
    llassert(sCurFBO == mFBO);
    llassert(sBoundTarget == this);

    if (gGLManager.mIsNativeVulkan)
    {
        LLRenderTarget* prev = mPreviousRT;
        if (sWolfVkNativeOwner)
        {
            sWolfVkNativeOwner->onFlushTarget(*this, prev);
        }
        if (mGenerateMipMaps == LLTexUnit::TMG_AUTO && !mTex.empty() && LLImageGL::sWolfVkNativeStore)
        {
            S32 cur_filter = 0, cur_addr = LLTexUnit::TAM_MIRROR;
            LLImageGL::sWolfVkNativeStore->getSamplerOptions(mTex[0], cur_filter, cur_addr);
            LLImageGL::sWolfVkNativeStore->setSamplerOptions(mTex[0], LLTexUnit::TFO_TRILINEAR, cur_addr);
            LLImageGL::sWolfVkNativeStore->generateMipmaps(mTex[0]);
        }
        if (prev)
        {
            sBoundTarget = prev->mPreviousRT;
            prev->bindTarget();
        }
        else
        {
            sBoundTarget = nullptr;
            sCurFBO = 0;
            sCurResX = gGLViewport[2];
            sCurResY = gGLViewport[3];
        }
        return;
    }

    if (mGenerateMipMaps == LLTexUnit::TMG_AUTO)
    {
        LL_PROFILE_GPU_ZONE("rt generate mipmaps");
        bindTexture(0, 0, LLTexUnit::TFO_TRILINEAR);
        glGenerateMipmap(GL_TEXTURE_2D);
    }

    if (mPreviousRT)
    {
        // a bit hacky -- pop the RT stack back two frames and push
        // the previous frame back on to play nice with the GL state machine
        sBoundTarget = mPreviousRT->mPreviousRT;
        mPreviousRT->bindTarget();
    }
    else
    {
        sBoundTarget = nullptr;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        sCurFBO = 0;
        glViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);
        sCurResX = gGLViewport[2];
        sCurResY = gGLViewport[3];
        glReadBuffer(GL_BACK);
        glDrawBuffer(GL_BACK);
    }
}

bool LLRenderTarget::isComplete() const
{
    return !mTex.empty() || mDepth;
}

void LLRenderTarget::getViewport(S32* viewport)
{
    viewport[0] = 0;
    viewport[1] = 0;
    viewport[2] = mResX;
    viewport[3] = mResY;
}

bool LLRenderTarget::isBoundInStack() const
{
    LLRenderTarget* cur = sBoundTarget;
    while (cur && cur != this)
    {
        cur = cur->mPreviousRT;
    }

    return cur == this;
}

void LLRenderTarget::swapFBORefs(LLRenderTarget& other)
{
    // Must be initialized
    llassert(mFBO);
    llassert(other.mFBO);

    // Must be unbound
    // *NOTE: mPreviousRT can be non-null even if this target is unbound - presumably for debugging purposes?
    llassert(sCurFBO != mFBO);
    llassert(sCurFBO != other.mFBO);
    llassert(!isBoundInStack());
    llassert(!other.isBoundInStack());

    // Must be same type
    llassert(sUseFBO == other.sUseFBO);
    llassert(mResX == other.mResX);
    llassert(mResY == other.mResY);
    llassert(mInternalFormat == other.mInternalFormat);
    llassert(mTex.size() == other.mTex.size());
    llassert(mDepth == other.mDepth);
    llassert(mUseDepth == other.mUseDepth);
    llassert(mGenerateMipMaps == other.mGenerateMipMaps);
    llassert(mMipLevels == other.mMipLevels);
    llassert(mUsage == other.mUsage);

    std::swap(mFBO, other.mFBO);
    std::swap(mTex, other.mTex);
    if (gGLManager.mIsNativeVulkan && sWolfVkNativeOwner)
    {
        sWolfVkNativeOwner->onTargetChanged(*this);
        sWolfVkNativeOwner->onTargetChanged(other);
    }
}
