/**
 * @file llrendertarget.h
 * @brief Off screen render target abstraction.  Loose wrapper for GL_EXT_framebuffer_objects.
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

#ifndef LL_LLRENDERTARGET_H
#define LL_LLRENDERTARGET_H

// LLRenderTarget is unavailible on the mapserver since it uses FBOs.

#include "llgl.h"
#include "llrender.h"
#include <vector>   // [G8 2026-11-11] mWolfVkProvidedNames

/*
 Wrapper around OpenGL framebuffer objects for use in render-to-texture

 SAMPLE USAGE:

    LLRenderTarget target;

    ...

    //allocate a 256x256 RGBA render target with depth buffer
    target.allocate(256,256,GL_RGBA,TRUE);

    //render to contents of offscreen buffer
    target.bindTarget();
    target.clear();
    ... <issue drawing commands> ...
    target.flush();

    ...

    //use target as a texture
    gGL.getTexUnit(INDEX)->bind(&target);
    ... <issue drawing commands> ...

*/

class LLRenderTarget
{
public:
    // Whether or not to use FBO implementation
    static bool sUseFBO;
    // [AMD PERF 2026-11-08] testing (WolfDebugGLDepth32F, set by LLPipeline before it allocates): GL's own depth buffers as
    // GL_DEPTH_COMPONENT32F like the Vulkan-shared one, to measure that format's cost in OpenGL mode alone
    static bool sWolfDepth32F;
    static U32 sBytesAllocated;
    static U32 sCurFBO;

    // [VULKAN G-BUFFER 2026-11-08] A target marked shared gets its textures from this provider instead of
    // LLImageGL::setManualImage: GL textures over memory the Vulkan renderer exported (wolfvkgbuffer.cpp), so Vulkan
    // can draw into the same G-buffer. The provider owns those textures (release() tells it, never deletes them);
    // colour formats it cannot share come back false and the target falls back to its own texture.
    struct WolfVkProvider
    {
        virtual ~WolfVkProvider() = default;
        virtual bool colorTexture(LLRenderTarget& target, U32 index, U32& color_fmt, U32& tex) = 0;   // may change color_fmt (GL_RGB16F -> GL_RGBA16F)
        virtual bool depthTexture(LLRenderTarget& target, U32& tex) = 0;                             // GL_DEPTH_COMPONENT32F
        virtual void released(LLRenderTarget& target) = 0;
    };
    static WolfVkProvider* sWolfVkProvider;
    // [VULKAN G-BUFFER S2 2026-11-26] called first by bindTarget / clear / flush while Vulkan holds the shared G-buffer
    // (null otherwise): Vulkan hands the images back to OpenGL before OpenGL changes or touches a target
    static void (*sWolfVkBeforeTargetChange)();
    // [NATIVE VULKAN 2026-11-26] Native Vulkan owner for LLRenderTarget when gGLManager.mIsNativeVulkan is true.
    struct WolfVkNativeOwner
    {
        virtual ~WolfVkNativeOwner() = default;
        virtual void onTargetChanged(LLRenderTarget& target) = 0;
        virtual void onShareDepth(LLRenderTarget& src, LLRenderTarget& dst) = 0;
        virtual void onBindTarget(LLRenderTarget& target) = 0;
        virtual void onFlushTarget(LLRenderTarget& target, LLRenderTarget* previous) = 0;
        virtual void onReleaseTarget(LLRenderTarget& target) = 0;
    };
    static WolfVkNativeOwner* sWolfVkNativeOwner;
    void setWolfVkShared(bool shared) { mWolfVkShared = shared; }
    bool isWolfVkShared() const { return mWolfVkShared; }

    // [VK S6 REGISTRY 2026-11-26] Any number of targets, each with its own provider (the G-buffer, the shadow maps, the
    // lighting and post targets ...). The single pointer above stays as the provider of targets marked with
    // setWolfVkShared(true) that have none of their own, so the G-buffer code is unchanged.
    //   wolfVkRegister(target, provider)   target takes its textures from provider at its NEXT allocation (a target
    //                                      already allocated keeps what it has until release()/allocate(); the caller
    //                                      re-allocates it, as for setWolfVkShared). provider null = the target has
    //                                      no provider of its own any more (same rule: from its next allocation).
    //   wolfVkUnregisterProvider(provider) the provider is going away: every target forgets it at once. Textures it
    //                                      had provided stay the provider's to delete (the target never deletes
    //                                      them and no longer calls released()), so the provider must make sure the
    //                                      targets are released or re-allocated before OpenGL draws to them again.
    //                                      A provider registered here MUST call it before it is destroyed; one that
    //                                      was registered and then replaced by another on a target is not told of
    //                                      that target's release() either (re-allocate the target before replacing).
    //   wolfVkProvider()                   the provider this target would ask now: its own, else sWolfVkProvider when
    //                                      marked shared, else null.
    // A provider sees the target it is asked for (size via getWidth/getHeight, wolfVkMipLevels(), the attachment index
    // and format), so one provider object can serve several targets or one each. Main thread only.
    static void wolfVkRegister(LLRenderTarget& target, WolfVkProvider* provider);
    static void wolfVkUnregisterProvider(WolfVkProvider* provider);
    WolfVkProvider* wolfVkProvider() const { return mWolfVkProvider ? mWolfVkProvider : (mWolfVkShared ? sWolfVkProvider : nullptr); }
    // Levels a provided colour texture must have: 1, or the full chain when the target generates mipmaps
    // (allocate(): 1 + floor(log2(max(width, height)))).
    U32 wolfVkMipLevels() const { return mGenerateMipMaps != LLTexUnit::TMG_NONE ? mMipLevels : 1; }
    static U32 sCurResX;
    static U32 sCurResY;


    LLRenderTarget();
    ~LLRenderTarget();

    //allocate resources for rendering
    //must be called before use
    //multiple calls will release previously allocated resources
    // resX - width
    // resY - height
    // color_fmt - GL color format (e.g. GL_RGB)
    // depth - if true, allocate a depth buffer
    // usage - deprecated, should always be TT_TEXTURE
    bool allocate(U32 resx, U32 resy, U32 color_fmt, bool depth = false, LLTexUnit::eTextureType usage = LLTexUnit::TT_TEXTURE, LLTexUnit::eTextureMipGeneration generateMipMaps = LLTexUnit::TMG_NONE);

    //resize existing attachments to use new resolution and color format
    // CAUTION: if the GL runs out of memory attempting to resize, this render target will be undefined
    // DO NOT use for screen space buffers or for scratch space for an image that might be uploaded
    // DO use for render targets that resize often and aren't likely to ruin someone's day if they break
    void resize(U32 resx, U32 resy);

    //point this render target at a particular LLImageGL
    //   Intended usage:
    //      LLRenderTarget target;
    //      target.addColorAttachment(image);
    //      target.bindTarget();
    //      < issue GL calls>
    //      target.flush();
    //      target.releaseColorAttachment();
    //
    // attachment -- LLImageGL to render into
    // use_name -- optional texture name to target instead of attachment->getTexName()
    // NOTE: setColorAttachment and releaseColorAttachment cannot be used in conjuction with
    // addColorAttachment, allocateDepth, resize, etc.
    void setColorAttachment(LLImageGL* attachment, LLGLuint use_name = 0);

    // detach from current color attachment
    void releaseColorAttachment();

    //add color buffer attachment
    //limit of 4 color attachments per render target
    bool addColorAttachment(U32 color_fmt);

    //allocate a depth texture
    bool allocateDepth();

    //share depth buffer with provided render target
    void shareDepthBuffer(LLRenderTarget& target);

    //free any allocated resources
    //safe to call redundantly
    // asserts that this target is not currently bound or present in the RT stack
    void release();

    //bind target for rendering
    //applies appropriate viewport
    //  If an LLRenderTarget is currently bound, stores a reference to that LLRenderTarget
    //  and restores previous binding on flush() (maintains a stack of Render Targets)
    //  Asserts that this target is not currently bound in the stack
    void bindTarget();

    //clear render targer, clears depth buffer if present,
    //uses scissor rect if in copy-to-texture mode
    // asserts that this target is currently bound
    void clear(U32 mask = 0xFFFFFFFF);

    //get applied viewport
    void getViewport(S32* viewport);

    //get X resolution
    U32 getWidth() const { return mResX; }

    //get Y resolution
    U32 getHeight() const { return mResY; }

    LLTexUnit::eTextureType getUsage(void) const { return mUsage; }

    U32 getTexture(U32 attachment = 0) const;
    U32 getNumTextures() const;

    U32 getDepth(void) const { return mDepth; }
    U32 getFBO() const { return mFBO; }   // [WolfViewer 2026-10-13] for reading single pixels (WolfEnvironmentRender::verifyFrame)

    void bindTexture(U32 index, S32 channel, LLTexUnit::eTextureFilterOptions filter_options = LLTexUnit::TFO_BILINEAR);

    //flush rendering operations
    //must be called when rendering is complete
    //should be used 1:1 with bindTarget
    // call bindTarget once, do all your rendering, call flush once
    // If an LLRenderTarget was bound when bindTarget was called, binds that RenderTarget for rendering (maintains RT stack)
    // asserts  that this target is currently bound
    void flush();

    //Returns TRUE if target is ready to be rendered into.
    //That is, if the target has been allocated with at least
    //one renderable attachment (i.e. color buffer, depth buffer).
    bool isComplete() const;

    // Returns true if this RenderTarget is bound somewhere in the stack
    bool isBoundInStack() const;

    static LLRenderTarget* getCurrentBoundTarget() { return sBoundTarget; }

    // *HACK
    void swapFBORefs(LLRenderTarget& other);

    static LLRenderTarget* sBoundTarget;

protected:
    U32 mResX;
    U32 mResY;
    std::vector<U32> mTex;
    std::vector<U32> mInternalFormat;
    U32 mFBO;
    LLRenderTarget* mPreviousRT = nullptr;

    U32 mDepth;
    bool mUseDepth;
    bool mWolfVkShared = false;          // [VULKAN G-BUFFER 2026-11-08] textures come from sWolfVkProvider
    // ... and the names of those it did provide: release() hands them back instead of deleting them, and deletes the
    // rest. [G8 2026-11-11] Per texture, not one flag: a target the provider shared only in part (a colour format it
    // cannot share, or no shared depth) has textures of its own as well, which one flag left undeleted (leaked).
    std::vector<U32> mWolfVkProvidedNames;
    WolfVkProvider* mWolfVkProvider = nullptr;     // [VK S6 REGISTRY 2026-11-26] this target's own provider (wolfVkRegister)
    WolfVkProvider* mWolfVkProvidedBy = nullptr;   // ... and the provider mWolfVkProvidedNames came from (told at release())
    LLTexUnit::eTextureMipGeneration mGenerateMipMaps;
    U32 mMipLevels;

    LLTexUnit::eTextureType mUsage;
};

#endif

