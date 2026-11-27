/**
 * @file wolfvkshadow.cpp
 * @brief The shadow maps as Vulkan-owned depth images that Vulkan draws into (wolfvkshadow.h).
 *
 * [W114-VULKAN S5 2026-11-26] Stage S5.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer Source Code
 * Copyright (C) 2026, IntelligentWolf Ltd
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"
#include "wolfvkshadow.h"
#include "pipeline.h"
#include "llgl.h"
#include "llglheaders.h"
#if LL_SDL
#include "SDL2/SDL_video.h"   // SDL_GL_GetProcAddress
#endif
#if LL_WINDOWS
#include <windows.h>
#endif

namespace
{
    // Source: GL/glext.h GL_HANDLE_TYPE_OPAQUE_FD_EXT 0x9586, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT 0x9587 (the lines cited in
    // wolfvkshare.h's header; the same constants as wolfvkgbuffer.cpp:35-36)
    const GLenum WOLF_GL_HANDLE_TYPE_OPAQUE_FD_EXT = 0x9586;
    const GLenum WOLF_GL_HANDLE_TYPE_OPAQUE_WIN32_EXT = 0x9587;

    // [D24] Shadow's decision: the maps are D24 where the device supports it (OpenGL's own are GL_DEPTH_COMPONENT24,
    // llrendertarget.cpp allocateDepth), else D32_SFLOAT. The D24 pair - GL_DEPTH_COMPONENT24 storage imported over a
    // VK_FORMAT_X8_D24_UNORM_PACK32 image - has not been proven by the share probe (S6's probe proved D32F both ways,
    // ~/.cache/shadow-scratch/vk-s6/probe/RESULTS.md) and WolfVkSharedImage::roundTrip does not decode that format
    // (wolfvkshare.cpp decodeTexel), so the branch below is complete but is not taken until the probe says "exact" and
    // this constant is turned on with it. With it on, a device that exports the format gets D24 maps, each proven by its
    // own round trip when it is made; a failed round trip puts the session back on D32_SFLOAT.
    const bool sD24ProvenByProbe = false;

    // No depth bias goes with either format: no shadow pass enables GL_POLYGON_OFFSET_FILL. LLPipeline::renderShadow
    // (pipeline.cpp) enables GL_CULL_FACE and, for the sun, GL_DEPTH_CLAMP only; LLDrawPoolTree::beginShadowPass /
    // endShadowPass (lldrawpooltree.cpp) call glPolygonOffset but nothing enables the offset; LLDrawPoolTerrain::
    // renderShadow has its LLGLEnable offset commented out (lldrawpoolterrain.cpp). So the stored depth is the plain
    // window z in both renderers, and the only difference between D32_SFLOAT and OpenGL's D24 is the rounding of that
    // value when it is stored.
    WolfVkImage::Format depthFormat(bool d24)
    {
        if (!d24) return WolfVkImage::formatFor(GL_DEPTH_COMPONENT32F, GL_FLOAT);   // VK_FORMAT_D32_SFLOAT (wolfvkimage.cpp formatFor)
        // Source: vulkan_core.h VK_FORMAT_X8_D24_UNORM_PACK32 ("a 32-bit format that has 24 unsigned normalized bits in
        // the depth component and, optionally, 8 bits that are unused", Vulkan spec 49.1.1 "Compatible Formats ...")
        WolfVkImage::Format f;
        f.mFormat = VK_FORMAT_X8_D24_UNORM_PACK32;
        f.mTexelBytes = 4;
        f.mSourceChannels = 1;
        f.mSourceBytesPerChannel = 4;
        f.mDepth = true;
        return f;
    }
    GLenum glDepthFormat(bool d24) { return d24 ? GL_DEPTH_COMPONENT24 : GL_DEPTH_COMPONENT32F; }
    // sampled by the lighting passes, drawn into as the depth attachment, copied out by a dump or read-back
    const VkImageUsageFlags sDepthUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    // the GL_EXT_semaphore entry points, fetched by name as wolfvkgbuffer.cpp does (each file keeps its own table)
    PFNGLGENSEMAPHORESEXTPROC sGenSemaphoresEXT = nullptr;
    PFNGLDELETESEMAPHORESEXTPROC sDeleteSemaphoresEXT = nullptr;
    PFNGLWAITSEMAPHOREEXTPROC sWaitSemaphoreEXT = nullptr;
    PFNGLSIGNALSEMAPHOREEXTPROC sSignalSemaphoreEXT = nullptr;
#if LL_WINDOWS
    PFNGLIMPORTSEMAPHOREWIN32HANDLEEXTPROC sImportSemaphoreWin32HandleEXT = nullptr;
#else
    PFNGLIMPORTSEMAPHOREFDEXTPROC sImportSemaphoreFdEXT = nullptr;
#endif
    bool sSemaphoreFunctionsLoaded = false;

    void* glProc(const char* name)
    {
#if LL_SDL
        return SDL_GL_GetProcAddress(name);
#elif LL_WINDOWS
        return reinterpret_cast<void*>(wglGetProcAddress(name));
#else
        (void)name;
        return nullptr;
#endif
    }

    bool loadSemaphoreFunctions()
    {
        if (!sSemaphoreFunctionsLoaded)
        {
            sSemaphoreFunctionsLoaded = true;
            sGenSemaphoresEXT = reinterpret_cast<PFNGLGENSEMAPHORESEXTPROC>(glProc("glGenSemaphoresEXT"));
            sDeleteSemaphoresEXT = reinterpret_cast<PFNGLDELETESEMAPHORESEXTPROC>(glProc("glDeleteSemaphoresEXT"));
            sWaitSemaphoreEXT = reinterpret_cast<PFNGLWAITSEMAPHOREEXTPROC>(glProc("glWaitSemaphoreEXT"));
            sSignalSemaphoreEXT = reinterpret_cast<PFNGLSIGNALSEMAPHOREEXTPROC>(glProc("glSignalSemaphoreEXT"));
#if LL_WINDOWS
            sImportSemaphoreWin32HandleEXT = reinterpret_cast<PFNGLIMPORTSEMAPHOREWIN32HANDLEEXTPROC>(glProc("glImportSemaphoreWin32HandleEXT"));
#else
            sImportSemaphoreFdEXT = reinterpret_cast<PFNGLIMPORTSEMAPHOREFDEXTPROC>(glProc("glImportSemaphoreFdEXT"));
#endif
        }
#if LL_WINDOWS
        return sGenSemaphoresEXT && sDeleteSemaphoresEXT && sWaitSemaphoreEXT && sSignalSemaphoreEXT && sImportSemaphoreWin32HandleEXT;
#else
        return sGenSemaphoresEXT && sDeleteSemaphoresEXT && sWaitSemaphoreEXT && sSignalSemaphoreEXT && sImportSemaphoreFdEXT;
#endif
    }

    std::string glErrorsHere()
    {
        std::string s;
        for (GLenum e = glGetError(); e != GL_NO_ERROR; e = glGetError())
        {
            if (!s.empty()) s += ", ";
            s += llformat("0x%x", e);
        }
        return s;
    }

    const VkPipelineStageFlags sDepthStages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags sDepthAccess = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
}

WolfVkShadow::~WolfVkShadow()
{
    destroy();
}

const char* WolfVkShadow::formatName() const
{
    return WolfVkImage::formatName(depthFormat(mD24).mFormat);
}

bool WolfVkShadow::init(WolfVkDevice& device, bool self_test, std::string& error)
{
    destroy();
    mDevice = &device;
    mSelfTest = self_test;
    std::string why;
    if (!WolfVkShare::available(device, why)) { error = why; mDevice = nullptr; return false; }
    if (!loadSemaphoreFunctions()) { error = "the OpenGL semaphore entry points are missing"; mDevice = nullptr; return false; }
    bool dedicated_only = false;
    std::string d24_error;
    mD24 = sD24ProvenByProbe && WolfVkImage::externalFormatSupported(device, depthFormat(true), sDepthUsage, dedicated_only, d24_error);
    if (!mD24 && !WolfVkImage::externalFormatSupported(device, depthFormat(false), sDepthUsage, dedicated_only, error)) { mDevice = nullptr; return false; }

    WolfVkDevice::ExternalHandle h1, h2;
    if (!device.createExportedSemaphore(mGlPassDone, h1, error)) { destroy(); return false; }
    if (!device.createExportedSemaphore(mVkPassDone, h2, error)) { device.closeExternalHandle(h1); destroy(); return false; }
    glErrorsHere();
    GLuint sems[2] = { 0, 0 };
    sGenSemaphoresEXT(2, sems);
    mGlSemGlPassDone = sems[0];
    mGlSemVkPassDone = sems[1];
#if LL_WINDOWS
    sImportSemaphoreWin32HandleEXT(mGlSemGlPassDone, WOLF_GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, h1.mWin32);
    sImportSemaphoreWin32HandleEXT(mGlSemVkPassDone, WOLF_GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, h2.mWin32);
#else
    sImportSemaphoreFdEXT(mGlSemGlPassDone, WOLF_GL_HANDLE_TYPE_OPAQUE_FD_EXT, h1.mFd);
    sImportSemaphoreFdEXT(mGlSemVkPassDone, WOLF_GL_HANDLE_TYPE_OPAQUE_FD_EXT, h2.mFd);
#endif
    const std::string e = glErrorsHere();
    if (!e.empty())
    {
        error = "glImportSemaphore (shadow maps): " + e;
        device.closeExternalHandle(h1);
        device.closeExternalHandle(h2);
        destroy();
        return false;
    }
    mAvailable = true;
    LL_INFOS("Vulkan") << "Shadow map sharing ready: " << formatName() << " exportable; pass semaphores made" << LL_ENDL;
    return true;
}

void WolfVkShadow::forEachTarget(const std::function<void(LLRenderTarget&)>& fn)
{
    // Source: pipeline.h RenderTargetPack::shadow[4] (mMainRT, mAuxillaryRT = the reflection probes' pack, mHeroProbeRT =
    // the mirrors' pack: pipeline.cpp allocateScreenBufferInternal) and LLPipeline::mSpotShadow[2]
    LLPipeline::RenderTargetPack* packs[3] = { &gPipeline.mMainRT, &gPipeline.mAuxillaryRT, &gPipeline.mHeroProbeRT };
    for (LLPipeline::RenderTargetPack* pack : packs)
    {
        for (LLRenderTarget& t : pack->shadow) fn(t);
    }
    for (LLRenderTarget& t : gPipeline.mSpotShadow) fn(t);
}

void WolfVkShadow::registerTargets()
{
    if (!mAvailable || mRegistered) return;
    mRegistered = true;
    // a target asks its provider at its next allocation only, and LLRenderTarget::allocate returns at once for a target
    // that already has the size asked for (llrendertarget.cpp allocate), so each map is released here; the pipeline
    // allocates them again in allocateShadowBuffer (the probe packs' through allocateScreenBufferInternal's recursion)
    forEachTarget([this](LLRenderTarget& t)
    {
        LLRenderTarget::wolfVkRegister(t, this);
        t.release();
    });
}

void WolfVkShadow::destroy()
{
    bool idle = !mDevice || !mDevice->hasDevice() || mDevice->waitIdle();
    if (idle && gGLManager.mInited && mDevice) idle = WolfVkShare::glFinishBounded(mDevice, "shadow maps destroy");
    const bool hung = !idle;
    if (mRegistered)
    {
        mRegistered = false;
        // every map goes back to a texture of OpenGL's own: released while this provider is still theirs (released()
        // drops the image), then forgotten. The framebuffer of a released target names no texture.
        if (gGLManager.mInited) forEachTarget([](LLRenderTarget& t) { t.release(); });
        LLRenderTarget::wolfVkUnregisterProvider(this);
    }
    mCurrent = nullptr;
    // on a hung device the images are left to it (WolfVkSharedImage::destroy defers through the frame slots, which never
    // run again there), as wolfvkgbuffer.cpp destroy does
    mMaps.clear();
    if (gGLManager.mInited && sDeleteSemaphoresEXT && !hung)
    {
        GLuint sems[2] = { mGlSemGlPassDone, mGlSemVkPassDone };
        if (sems[0] || sems[1]) sDeleteSemaphoresEXT(2, sems);
    }
    mGlSemGlPassDone = mGlSemVkPassDone = 0;
    if (mDevice && mDevice->hasDevice() && !hung)
    {
        if (mGlPassDone) gVk.vkDestroySemaphore(mDevice->device(), mGlPassDone, nullptr);
        if (mVkPassDone) gVk.vkDestroySemaphore(mDevice->device(), mVkPassDone, nullptr);
    }
    mGlPassDone = mVkPassDone = VK_NULL_HANDLE;
    mGlSignalled = mVkSignalled = false;
    destroyRewrite();
    mBrokenReason.clear();
    mDisabledReason.clear();
    mSelfTestResult.clear();
    mAvailable = false;
    mD24 = false;
    mDevice = nullptr;
}

void WolfVkShadow::destroyRewrite()
{
    if (gGLManager.mInited)
    {
        if (mRewriteTex) glDeleteTextures(1, &mRewriteTex);
        if (mRewriteProgram) glDeleteProgram(mRewriteProgram);
        if (mRewriteVao) glDeleteVertexArrays(1, &mRewriteVao);
    }
    mRewriteTex = mRewriteProgram = mRewriteVao = 0;
    mRewriteW = mRewriteH = 0;
    mRewriteTexLoc = -1;
    mRewriteD24 = false;
}

bool WolfVkShadow::idleBothAPIs(const char* what)
{
    // as WolfVkGBuffer::idleBothAPIs: both renderers have finished with an image before it is destroyed; bounded
    if (!mDevice || !mDevice->hasDevice()) return true;
    if (mDevice->waitIdle() && (!gGLManager.mInited || WolfVkShare::glFinishBounded(mDevice, what))) return true;
    mAvailable = false;
    return false;
}

WolfVkShadow::Map* WolfVkShadow::find(const LLRenderTarget* target)
{
    for (std::unique_ptr<Map>& m : mMaps) if (m->mTarget == target) return m.get();
    return nullptr;
}

WolfVkSharedImage* WolfVkShadow::shared(const LLRenderTarget* target)
{
    Map* m = find(target);
    return (m && m->mDepth.isValid()) ? &m->mDepth : nullptr;
}

// ---- LLRenderTarget::WolfVkProvider --------------------------------------------------------------------------------

bool WolfVkShadow::colorTexture(LLRenderTarget& /*target*/, U32 /*index*/, U32& /*color_fmt*/, U32& /*tex*/)
{
    // a shadow map is allocate(w, h, 0, true): LLRenderTarget::allocate adds no colour attachment for format 0
    // (llrendertarget.cpp allocate). A registered target that is ever given one keeps OpenGL's own texture for it, and
    // selectBound() then refuses the target (a Vulkan target of ours is depth only).
    return false;
}

bool WolfVkShadow::depthTexture(LLRenderTarget& target, U32& tex)
{
    if (!mAvailable || !mDevice) return false;
    const U32 w = target.getWidth(), h = target.getHeight();
    if (w == 0 || h == 0) return false;
    Map* old = find(&target);
    if (old)
    {
        // allocate() released the target first, so released() has already dropped its map; one that is still here was
        // never given back (it cannot be in use by the target any more)
        if (!idleBothAPIs("shadow map remake")) return false;
        released(target);
    }
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        std::unique_ptr<Map> map(new Map());
        map->mTarget = &target;
        map->mWidth = w;
        map->mHeight = h;
        std::string error;
        bool ok = map->mDepth.createKind(*mDevice, WolfVkImage::Kind::TEXTURE_2D, depthFormat(mD24), w, h, 1, 1, sDepthUsage, glDepthFormat(mD24), error);
        // the first hand-over: the fresh image in the layout both renderers use it in, OpenGL's (wolfvkshare.h handToGL)
        ok = ok && map->mDepth.handToGL(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, error);
        if (ok && mSelfTest)
        {
            // OpenGL writes it, Vulkan reads it back, through the pass semaphores (nothing is pending on either between
            // brackets); as WolfVkGBuffer::createImage
            std::string line;
            ok = map->mDepth.roundTrip(mGlSemGlPassDone, mGlSemVkPassDone, mGlPassDone, mVkPassDone, false, line);
            mGlSignalled = mVkSignalled = false;
            if (!mSelfTestResult.empty()) mSelfTestResult += "; ";
            mSelfTestResult += line;
            if (ok) LL_INFOS_ONCE("Vulkan") << "Shadow map self-test: " << line << LL_ENDL;
            else error = "self-test failed: " + line;
        }
        if (ok)
        {
            tex = map->mDepth.glTexture();
            LL_INFOS("Vulkan") << "Shadow map shared: " << w << "x" << h << " " << formatName() << LL_ENDL;
            mMaps.push_back(std::move(map));
            return true;
        }
        map.reset();   // nothing of it was handed out
        if (mD24)
        {
            // the D24 pair failed on this device: the session's maps are D32_SFLOAT from here (the maps already made as
            // D24 passed their own round trip and stay until their targets are allocated again)
            LL_WARNS("Vulkan") << "Shadow map as X8_D24_UNORM_PACK32 failed (" << error << "): D32_SFLOAT from here" << LL_ENDL;
            mD24 = false;
            bool dedicated_only = false;
            if (WolfVkImage::externalFormatSupported(*mDevice, depthFormat(false), sDepthUsage, dedicated_only, error)) continue;
        }
        LL_WARNS("Vulkan") << "Shadow map could not be shared (" << error << "): shadow map sharing ends, OpenGL draws the shadow passes" << LL_ENDL;
        mDisabledReason = error;
        mAvailable = false;
        return false;
    }
    return false;
}

void WolfVkShadow::released(LLRenderTarget& target)
{
    for (auto it = mMaps.begin(); it != mMaps.end(); ++it)
    {
        if ((*it)->mTarget != &target) continue;
        if (mCurrent == it->get()) mCurrent = nullptr;
        (*it)->mVkTarget.reset();   // deferred by its own destroy (frame slot), like the image
        if (idleBothAPIs("shadow map release"))
        {
            mMaps.erase(it);
        }
        else
        {
            // hung: the image stays with the abandoned device; the entry no longer answers for the target
            (*it)->mTarget = nullptr;
        }
        return;
    }
}

// ---- the bracket ---------------------------------------------------------------------------------------------------

bool WolfVkShadow::selectBound()
{
    if (!mAvailable || broken()) return false;
    LLRenderTarget* bound = LLRenderTarget::getCurrentBoundTarget();
    if (mCurrent && mCurrent->mTarget == bound && bound) return true;
    Map* m = bound ? find(bound) : nullptr;
    // depth only, at the size the map was made for (a target given a colour attachment or another size is not ours)
    if (!m || !m->mDepth.isValid() || bound->getNumTextures() != 0 || bound->getWidth() != m->mWidth || bound->getHeight() != m->mHeight) return false;
    mCurrent = m;
    return true;
}

bool WolfVkShadow::prepare()
{
    if (!mAvailable || !mCurrent || !mCurrent->mDepth.isValid()) return false;
    if (!mCurrent->mVkTarget)
    {
        mCurrent->mVkTarget.reset(new WolfVkRenderTarget());
        mCurrent->mVkTarget->init(*mDevice);
        mCurrent->mVkTarget->setExternalDepth(mCurrent->mDepth.image());   // depth only: the size is the image's
    }
    return true;
}

WolfVkRenderTarget* WolfVkShadow::target()
{
    return (mCurrent && mCurrent->mVkTarget) ? mCurrent->mVkTarget.get() : nullptr;
}

bool WolfVkShadow::glHandToVulkan()
{
    // [HAND-OVER 8.2k] as WolfVkGBuffer::glHandToVulkan: every false leaves gl_pass_done unsignalled, and the caller then
    // records and submits nothing
    if (!mAvailable || broken() || mGlSignalled) return false;
    if (!prepare()) return false;
    // the map, and the images the pass's draws sample (wolfvkbrackettarget.h WolfVkBracketExtras)
    std::vector<GLuint> textures{ mCurrent->mDepth.glTexture() };
    std::vector<GLenum> layouts{ WolfVkSharedImage::glLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) };
    mExtras.appendForSignal(textures, layouts);
    glErrorsHere();
    sSignalSemaphoreEXT(mGlSemGlPassDone, 0, nullptr, static_cast<GLuint>(textures.size()), textures.data(), layouts.data());
    const std::string e = glErrorsHere();
    if (!e.empty())
    {
        mExtras.cancel();
        mBrokenReason = "glSignalSemaphoreEXT (shadow gl_pass_done): GL error " + e;
        LL_WARNS("Vulkan") << "Shadow map hand-over: " << mBrokenReason << LL_ENDL;
        return false;
    }
    mGlSignalled = true;
    return true;
}

bool WolfVkShadow::glTakeBack()
{
    if (!mAvailable || !mVkSignalled || !mCurrent) return true;   // nothing Vulkan signalled is waiting to be taken
    std::vector<GLuint> textures{ mCurrent->mDepth.glTexture() };
    std::vector<GLenum> layouts{ WolfVkSharedImage::glLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) };
    mExtras.appendForWait(textures, layouts);
    glErrorsHere();
    sWaitSemaphoreEXT(mGlSemVkPassDone, 0, nullptr, static_cast<GLuint>(textures.size()), textures.data(), layouts.data());
    mVkSignalled = false;
    const std::string e = glErrorsHere();
    if (!e.empty())
    {
        mBrokenReason = "glWaitSemaphoreEXT (shadow vk_pass_done): GL error " + e;
        LL_WARNS("Vulkan") << "Shadow map hand-back: " << mBrokenReason << LL_ENDL;
        return false;
    }
    return true;
}

WolfVkDevice::ExtraSync WolfVkShadow::sync()
{
    WolfVkDevice::ExtraSync s;
    if (mGlSignalled)
    {
        s.mWaits.push_back(mGlPassDone);
        s.mWaitStages.push_back(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);   // the pass's first use of the map is its depth test
        mGlSignalled = false;
    }
    if (!mVkSignalled)
    {
        s.mSignals.push_back(mVkPassDone);
        mVkSignalled = true;
    }
    return s;
}

void WolfVkShadow::acquire(VkCommandBuffer cmd)
{
    if (!mCurrent) return;
    mCurrent->mDepth.acquireFromGL(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, sDepthStages, sDepthAccess);
    mExtras.acquire(cmd);
}

void WolfVkShadow::release(VkCommandBuffer cmd)
{
    if (!mCurrent) return;
    mCurrent->mDepth.releaseToGL(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    mExtras.release(cmd);
}

void WolfVkShadow::glAfterTakeBack()
{
#if LL_DARWIN
    // Not built for macOS, as WolfVkGBuffer::glRewriteDepth: Apple's GL has no glCopyImageSubData (GL 4.3) and no
    // GL_EXT_memory_object, so no map is ever shared there (WolfVkShare::available is false) and nothing reaches this.
    return;
#else
    // OpenGL writes every texel of the map's depth itself before it draws into it again (docs 8.2n, wolfvkgbuffer.h
    // glRewriteDepth: after Vulkan had written a shared depth, OpenGL's later depth tests against it went wrong until
    // OpenGL's depth unit had written the texels). A shadow pass has such draws while any caster is still left to
    // OpenGL in the middle of a map. The values do not change: a copy (glCopyImageSubData, OpenGL 4.6 18.3.3) drawn
    // back through gl_FragDepth with the test at GL_ALWAYS.
    if (!mCurrent || !mCurrent->mDepth.isValid()) return;
    const GLsizei w = static_cast<GLsizei>(mCurrent->mWidth), h = static_cast<GLsizei>(mCurrent->mHeight);
    if (!mRewriteProgram)
    {
        const char* vs_src = "#version 330 core\nvoid main(){ vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0, (gl_VertexID == 2) ? 3.0 : -1.0); gl_Position = vec4(p, 0.0, 1.0); }\n";
        const char* fs_src = "#version 330 core\nuniform sampler2D depthTex;\nvoid main(){ gl_FragDepth = texelFetch(depthTex, ivec2(gl_FragCoord.xy), 0).r; }\n";
        GLuint vs = glCreateShader(GL_VERTEX_SHADER); glShaderSource(vs, 1, &vs_src, nullptr); glCompileShader(vs);
        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(fs, 1, &fs_src, nullptr); glCompileShader(fs);
        mRewriteProgram = glCreateProgram();
        glAttachShader(mRewriteProgram, vs); glAttachShader(mRewriteProgram, fs); glLinkProgram(mRewriteProgram);
        glDeleteShader(vs); glDeleteShader(fs);
        GLint linked = GL_FALSE;
        glGetProgramiv(mRewriteProgram, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE)
        {
            LL_WARNS_ONCE("Vulkan") << "GL shadow depth rewrite program did not link" << LL_ENDL;
            glDeleteProgram(mRewriteProgram);
            mRewriteProgram = 0;
            return;
        }
        mRewriteTexLoc = glGetUniformLocation(mRewriteProgram, "depthTex");
        glGenVertexArrays(1, &mRewriteVao);
    }
    GLint saved_tex = 0, saved_prog = 0, saved_vao = 0, saved_active = 0, saved_func = GL_LESS, vp[4] = { 0, 0, 0, 0 };
    GLboolean saved_mask = GL_TRUE, cm[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
    glGetIntegerv(GL_ACTIVE_TEXTURE, &saved_active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved_tex);
    glGetIntegerv(GL_CURRENT_PROGRAM, &saved_prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &saved_vao);
    glGetIntegerv(GL_DEPTH_FUNC, &saved_func);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &saved_mask);
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    const GLboolean saved_test = glIsEnabled(GL_DEPTH_TEST), saved_scissor = glIsEnabled(GL_SCISSOR_TEST), saved_cull = glIsEnabled(GL_CULL_FACE);
    // the copy has the map's own internal format (glCopyImageSubData needs compatible formats, OpenGL 4.6 18.3.3)
    const uint32_t tex_w = mCurrent->mWidth, tex_h = mCurrent->mHeight;
    if (!mRewriteTex || mRewriteW != tex_w || mRewriteH != tex_h || mRewriteD24 != mD24)
    {
        if (mRewriteTex) glDeleteTextures(1, &mRewriteTex);
        glGenTextures(1, &mRewriteTex);
        glBindTexture(GL_TEXTURE_2D, mRewriteTex);
        glTexStorage2D(GL_TEXTURE_2D, 1, glDepthFormat(mD24), w, h);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        mRewriteW = tex_w; mRewriteH = tex_h;
        mRewriteD24 = mD24;
    }
    glErrorsHere();
    glCopyImageSubData(mCurrent->mDepth.glTexture(), GL_TEXTURE_2D, 0, 0, 0, 0, mRewriteTex, GL_TEXTURE_2D, 0, 0, 0, 0, w, h, 1);
    glUseProgram(mRewriteProgram);
    glBindTexture(GL_TEXTURE_2D, mRewriteTex);
    glUniform1i(mRewriteTexLoc, 0);
    glBindVertexArray(mRewriteVao);
    glViewport(0, 0, w, h);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    glDepthMask(GL_TRUE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    const std::string e = glErrorsHere();
    if (!e.empty()) LL_WARNS_ONCE("Vulkan") << "GL shadow depth rewrite: GL error " << e << LL_ENDL;
    glColorMask(cm[0], cm[1], cm[2], cm[3]);
    glDepthMask(saved_mask);
    glDepthFunc(static_cast<GLenum>(saved_func));
    if (!saved_test) glDisable(GL_DEPTH_TEST);
    if (saved_scissor) glEnable(GL_SCISSOR_TEST);
    if (saved_cull) glEnable(GL_CULL_FACE);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glBindVertexArray(static_cast<GLuint>(saved_vao));
    glUseProgram(static_cast<GLuint>(saved_prog));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(saved_tex));
    glActiveTexture(static_cast<GLenum>(saved_active));
#endif
}
