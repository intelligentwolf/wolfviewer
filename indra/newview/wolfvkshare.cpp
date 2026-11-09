/**
 * @file wolfvkshare.cpp
 * @brief Images and semaphores shared between the OpenGL context and the Vulkan device (see wolfvkshare.h).
 *
 * [SHARED IMAGES 2026-11-08]
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
#include "wolfvkshare.h"
#include "llgl.h"
#include "llglheaders.h"
#include <cstring>
#include <algorithm>

#if LL_SDL
#include "SDL2/SDL_video.h"   // SDL_GL_GetProcAddress, as llgl.cpp:64 GLH_EXT_GET_PROC_ADDRESS
#elif LL_WINDOWS
#include <windows.h>          // wglGetProcAddress
#endif

namespace
{
    // ---- the GL_EXT_memory_object / GL_EXT_semaphore entry points, fetched once -------------------------------------
    // typedefs: GL/glext.h (lines in wolfvkshare.h)
    struct GLShareFunctions
    {
        PFNGLCREATEMEMORYOBJECTSEXTPROC CreateMemoryObjectsEXT = nullptr;
        PFNGLDELETEMEMORYOBJECTSEXTPROC DeleteMemoryObjectsEXT = nullptr;
        PFNGLMEMORYOBJECTPARAMETERIVEXTPROC MemoryObjectParameterivEXT = nullptr;
        PFNGLTEXSTORAGEMEM2DEXTPROC TexStorageMem2DEXT = nullptr;
        PFNGLGENSEMAPHORESEXTPROC GenSemaphoresEXT = nullptr;
        PFNGLDELETESEMAPHORESEXTPROC DeleteSemaphoresEXT = nullptr;
        PFNGLWAITSEMAPHOREEXTPROC WaitSemaphoreEXT = nullptr;
        PFNGLSIGNALSEMAPHOREEXTPROC SignalSemaphoreEXT = nullptr;
#if LL_WINDOWS
        PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC ImportMemoryWin32HandleEXT = nullptr;
        PFNGLIMPORTSEMAPHOREWIN32HANDLEEXTPROC ImportSemaphoreWin32HandleEXT = nullptr;
#else
        PFNGLIMPORTMEMORYFDEXTPROC ImportMemoryFdEXT = nullptr;
        PFNGLIMPORTSEMAPHOREFDEXTPROC ImportSemaphoreFdEXT = nullptr;
#endif
        bool mLoaded = false;
        bool mComplete = false;
    };
    GLShareFunctions sGL;

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

#if !LL_DARWIN
    // [CI 2026-11-09] not built on macOS: WolfVkShare::available() returns before any extension check there (Apple's
    // OpenGL has no GL_EXT_memory_object), and an unused static function fails the macOS build (-Werror,
    // -Wunused-function; CI run 42603917436).
    bool glHasExtension(const char* name)
    {
#if LL_WINDOWS
        if (!glGetStringi) return false;   // run-time pointer on Windows (llglheaders.h:354)
#endif
        GLint count = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &count);
        for (GLint i = 0; i < count; ++i)
        {
            const char* ext = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
            if (ext && strcmp(ext, name) == 0) return true;
        }
        return false;
    }
#endif

    void loadGLFunctions()
    {
        if (sGL.mLoaded) return;
        sGL.mLoaded = true;
#define WOLF_GL_LOAD(member, name) sGL.member = reinterpret_cast<decltype(sGL.member)>(glProc(name)); if (!sGL.member) complete = false;
        bool complete = true;
        WOLF_GL_LOAD(CreateMemoryObjectsEXT, "glCreateMemoryObjectsEXT")
        WOLF_GL_LOAD(DeleteMemoryObjectsEXT, "glDeleteMemoryObjectsEXT")
        WOLF_GL_LOAD(MemoryObjectParameterivEXT, "glMemoryObjectParameterivEXT")
        WOLF_GL_LOAD(TexStorageMem2DEXT, "glTexStorageMem2DEXT")
        WOLF_GL_LOAD(GenSemaphoresEXT, "glGenSemaphoresEXT")
        WOLF_GL_LOAD(DeleteSemaphoresEXT, "glDeleteSemaphoresEXT")
        WOLF_GL_LOAD(WaitSemaphoreEXT, "glWaitSemaphoreEXT")
        WOLF_GL_LOAD(SignalSemaphoreEXT, "glSignalSemaphoreEXT")
#if LL_WINDOWS
        WOLF_GL_LOAD(ImportMemoryWin32HandleEXT, "glImportMemoryWin32HandleEXT")
        WOLF_GL_LOAD(ImportSemaphoreWin32HandleEXT, "glImportSemaphoreWin32HandleEXT")
#else
        WOLF_GL_LOAD(ImportMemoryFdEXT, "glImportMemoryFdEXT")
        WOLF_GL_LOAD(ImportSemaphoreFdEXT, "glImportSemaphoreFdEXT")
#endif
#undef WOLF_GL_LOAD
        sGL.mComplete = complete;
    }

    // GL/glext.h tokens (lines in wolfvkshare.h)
    const GLenum WOLF_GL_TEXTURE_TILING_EXT = 0x9580;
    const GLenum WOLF_GL_DEDICATED_MEMORY_OBJECT_EXT = 0x9581;
    const GLenum WOLF_GL_OPTIMAL_TILING_EXT = 0x9584;
    const GLenum WOLF_GL_HANDLE_TYPE_OPAQUE_FD_EXT = 0x9586;
    const GLenum WOLF_GL_HANDLE_TYPE_OPAQUE_WIN32_EXT = 0x9587;
    const GLenum WOLF_GL_LAYOUT_COLOR_ATTACHMENT_EXT = 0x958E;
    const GLenum WOLF_GL_LAYOUT_DEPTH_STENCIL_ATTACHMENT_EXT = 0x958F;

    const char* glErrorName(GLenum e)
    {
        switch (e)
        {
        case GL_INVALID_ENUM: return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE: return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
        case GL_OUT_OF_MEMORY: return "GL_OUT_OF_MEMORY";
        case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
        default: return "GL error";
        }
    }

    // every GL error since the last call, as text ("" when none)
    std::string glErrors()
    {
        std::string s;
        for (GLenum e = glGetError(); e != GL_NO_ERROR; e = glGetError())
        {
            if (!s.empty()) s += ", ";
            s += glErrorName(e);
        }
        return s;
    }

    // The GL state the copies and the depth pass touch, saved on construction and put back on destruction, so the
    // viewer's own tracking (LLGLState, LLRender) sees nothing changed.
    struct GLStateKeeper
    {
        GLint mReadFbo = 0, mDrawFbo = 0, mProgram = 0, mVao = 0, mActiveTexture = 0, mTexture2D = 0;
        GLint mViewport[4] = { 0, 0, 0, 0 };
        GLboolean mDepthTest = GL_FALSE, mDepthMask = GL_TRUE, mCull = GL_FALSE, mScissor = GL_FALSE, mBlend = GL_FALSE;
        GLint mDepthFunc = GL_LESS;
        GLboolean mColorMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
        GLfloat mDepthClear = 1.f;
        GLStateKeeper()
        {
            glGetFloatv(GL_DEPTH_CLEAR_VALUE, &mDepthClear);
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &mReadFbo);
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &mDrawFbo);
            glGetIntegerv(GL_CURRENT_PROGRAM, &mProgram);
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &mVao);
            glGetIntegerv(GL_ACTIVE_TEXTURE, &mActiveTexture);
            glActiveTexture(GL_TEXTURE0);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &mTexture2D);
            glGetIntegerv(GL_VIEWPORT, mViewport);
            mDepthTest = glIsEnabled(GL_DEPTH_TEST);
            glGetBooleanv(GL_DEPTH_WRITEMASK, &mDepthMask);
            glGetIntegerv(GL_DEPTH_FUNC, &mDepthFunc);
            mCull = glIsEnabled(GL_CULL_FACE);
            mScissor = glIsEnabled(GL_SCISSOR_TEST);
            mBlend = glIsEnabled(GL_BLEND);
            glGetBooleanv(GL_COLOR_WRITEMASK, mColorMask);
        }
        ~GLStateKeeper()
        {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(mReadFbo));
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(mDrawFbo));
            glUseProgram(static_cast<GLuint>(mProgram));
            glBindVertexArray(static_cast<GLuint>(mVao));
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(mTexture2D));
            glActiveTexture(static_cast<GLenum>(mActiveTexture));
            glViewport(mViewport[0], mViewport[1], mViewport[2], mViewport[3]);
            if (mDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
            glDepthMask(mDepthMask);
            glDepthFunc(static_cast<GLenum>(mDepthFunc));
            if (mCull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
            if (mScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
            if (mBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
            glColorMask(mColorMask[0], mColorMask[1], mColorMask[2], mColorMask[3]);
            glClearDepth(static_cast<GLclampd>(mDepthClear));
        }
    };

    // the depth copy: a full-screen triangle from gl_VertexID; the fragment writes deferredScreen's depth at
    // (gl_FragCoord - origin) into gl_FragDepth. GLSL 3.30 core: the viewer's context is a 3.3+ core profile (llgl.cpp)
    const char* const sDepthCopyVert =
        "#version 330 core\n"
        "void main()\n"
        "{\n"
        "    vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0, (gl_VertexID == 2) ? 3.0 : -1.0);\n"
        "    gl_Position = vec4(p, 0.0, 1.0);\n"
        "}\n";
    const char* const sDepthCopyFrag =
        "#version 330 core\n"
        "uniform sampler2D depthTex;\n"
        "uniform ivec2 origin;\n"
        "void main()\n"
        "{\n"
        "    gl_FragDepth = texelFetch(depthTex, ivec2(gl_FragCoord.xy) - origin, 0).r;\n"
        "}\n";
}

// ---------------------------------------------------------------------------------------------------------------

WolfVkShare::~WolfVkShare()
{
    destroy();
}

bool WolfVkShare::available(const WolfVkDevice& device, std::string& why)
{
#if LL_DARWIN
    (void)device;
    why = "Apple's OpenGL has no GL_EXT_memory_object";
    return false;
#else
    if (!device.externalSharing())
    {
        why = "the Vulkan device has no external memory/semaphore extensions";
        return false;
    }
    if (!glHasExtension("GL_EXT_memory_object") || !glHasExtension("GL_EXT_semaphore"))
    {
        why = "OpenGL lacks GL_EXT_memory_object / GL_EXT_semaphore";
        return false;
    }
#if LL_WINDOWS
    if (!glHasExtension("GL_EXT_memory_object_win32") || !glHasExtension("GL_EXT_semaphore_win32"))
    {
        why = "OpenGL lacks GL_EXT_memory_object_win32 / GL_EXT_semaphore_win32";
        return false;
    }
#else
    if (!glHasExtension("GL_EXT_memory_object_fd") || !glHasExtension("GL_EXT_semaphore_fd"))
    {
        why = "OpenGL lacks GL_EXT_memory_object_fd / GL_EXT_semaphore_fd";
        return false;
    }
#endif
    loadGLFunctions();
    if (!sGL.mComplete)
    {
        why = "an OpenGL memory-object or semaphore entry point is missing";
        return false;
    }
    return true;
#endif
}

bool WolfVkShare::init(WolfVkDevice& device, uint32_t width, uint32_t height, std::string& error)
{
    destroy();
    mDevice = &device;
    std::string why;
    if (!available(device, why))
    {
        error = why;
        mDevice = nullptr;
        return false;
    }
    // the three formats must be exportable (vkglshare: RGBA8 / D32F optimal, dedicated, on both drivers)
    bool dedicated_only = false;
    const VkImageUsageFlags colour_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;    // :2810, :2812, :2808
    const VkImageUsageFlags depth_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;   // :2813
    if (!WolfVkImage::externalFormatSupported(device, WolfVkImage::formatFor(GL_RGBA8, GL_UNSIGNED_BYTE), colour_usage, dedicated_only, error)) { destroy(); return false; }
    if (!WolfVkImage::externalFormatSupported(device, WolfVkImage::formatFor(GL_DEPTH_COMPONENT32F, GL_FLOAT), depth_usage, dedicated_only, error)) { destroy(); return false; }
    if (!createSemaphores(error)) { destroy(); return false; }
    if (!createDepthCopyProgram(error)) { destroy(); return false; }
    if (!resize(width, height, error)) { destroy(); return false; }
    LL_INFOS("Vulkan") << "Shared GL/Vulkan layers: " << width << "x" << height << " world + UI RGBA8, depth D32_SFLOAT; two exported semaphores" << LL_ENDL;
    return true;
}

bool WolfVkShare::resize(uint32_t width, uint32_t height, std::string& error)
{
    if (!mDevice) { error = "WolfVkShare not initialised"; return false; }
    if (width == 0 || height == 0) { error = "no size"; return false; }
    if (mWidth == width && mHeight == height && isValid()) return true;
    // GL may still be reading/writing the old layers of a frame in flight on its side; its context is in-order with
    // the viewer's calls, and the Vulkan side has finished (waitIdle) before the old images go.
    // [HANG GUARD 2026-11-08 8.2k] both waits bounded; if either runs out the old images are kept (something may still
    // use them) and the resize fails, which the caller turns into a fall-back through the device's hung flag
    if (!mDevice->waitIdle() || !glFinishBounded(mDevice, "shared layers resize"))
    {
        error = "the graphics card did not finish with the old layers (" + mDevice->hungReason() + ")";
        return false;
    }
    destroyLayer(mWorld);
    destroyLayer(mUI);
    destroyLayer(mDepth);
    mWidth = width;
    mHeight = height;
    if (!createLayer(mWorld, width, height, false, error)) return false;
    if (!createLayer(mUI, width, height, false, error)) return false;
    if (!createLayer(mDepth, width, height, true, error)) return false;
    return primeVulkan(error);
}

void WolfVkShare::destroy()
{
    // [HANG GUARD 2026-11-08 8.2k] decided by the waits themselves, as in WolfVkGBuffer::destroy (review: hung() was read
    // before waitIdle). GL may be waiting on the hung card too, so glFinishBounded is not started once Vulkan's wait failed.
    bool idle = !mDevice || !mDevice->hasDevice() || mDevice->waitIdle();
    if (idle && gGLManager.mInited && mDevice) idle = glFinishBounded(mDevice, "shared layers destroy");   // nothing shared yet without a device
    const bool hung = !idle;
    destroyLayer(mWorld);
    destroyLayer(mUI);
    destroyLayer(mDepth);
    destroyGLProgram();
    if (gGLManager.mInited && sGL.mComplete && !hung)
    {
        GLuint sems[2] = { mGlSemGlDone, mGlSemVkDone };
        if (sems[0] || sems[1]) sGL.DeleteSemaphoresEXT(2, sems);
    }
    mGlSemGlDone = mGlSemVkDone = 0;
    // [HANG GUARD 2026-11-08 8.2k] as WolfVkGBuffer::destroy: a hung device's submits may still name them
    // (VUID-vkDestroySemaphore-semaphore-05149), so they stay with the abandoned device
    if (mDevice && mDevice->hasDevice() && !hung)
    {
        if (mGlDone) gVk.vkDestroySemaphore(mDevice->device(), mGlDone, nullptr);
        if (mVkDone) gVk.vkDestroySemaphore(mDevice->device(), mVkDone, nullptr);
    }
    mGlDone = mVkDone = VK_NULL_HANDLE;
    mGlSignalled = mVkSignalled = false;
    mBrokenReason.clear();
    mWidth = mHeight = 0;
    mDevice = nullptr;
}

bool WolfVkShare::glFinishBounded(WolfVkDevice* device, const char* what)
{
    if (device && device->hung()) return false;
    // as llimagegl.cpp:1765-1768 syncToMainThread does it, with a timeout instead of GL_TIMEOUT_IGNORED. Tokens and
    // prototypes: GL/glcorearb.h:1516 GL_SYNC_GPU_COMMANDS_COMPLETE, :1519-1522 GL_ALREADY_SIGNALED / GL_TIMEOUT_EXPIRED /
    // GL_CONDITION_SATISFIED / GL_WAIT_FAILED, :1524 GL_SYNC_FLUSH_COMMANDS_BIT (so the fence itself reaches the card),
    // :1554 glClientWaitSync(sync, flags, GLuint64 timeout in nanoseconds)
    GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (!sync)
    {
        if (device) device->declareHung(std::string(what) + ": glFenceSync failed, OpenGL's progress cannot be checked");
        return false;
    }
    const GLenum r = glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, static_cast<GLuint64>(WolfVkDevice::FRAME_WAIT_LIMIT_NS));
    glDeleteSync(sync);
    if (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED) return true;
    if (device) device->declareHung(std::string(what) + (r == GL_TIMEOUT_EXPIRED ? ": OpenGL did not finish within 8 s" : ": glClientWaitSync failed"));
    return false;
}

// ---- layers --------------------------------------------------------------------------------------------------------

// ---- WolfVkSharedImage ----------------------------------------------------------------------------------------------

WolfVkSharedImage::~WolfVkSharedImage()
{
    destroy();
}

bool WolfVkSharedImage::create(WolfVkDevice& device, const WolfVkImage::Format& format, uint32_t width, uint32_t height, VkImageUsageFlags usage,
                               uint32_t gl_internal_format, std::string& error)
{
    destroy();
    if (!WolfVkShare::loadGL()) { error = "the OpenGL memory-object entry points are missing"; return false; }
    mDevice = &device;
    mImage.reset(new WolfVkImage());
    if (!mImage->createExternal(device, format, width, height, usage, error)) { destroy(); return false; }
    WolfVkDevice::ExternalHandle handle;
    if (!mImage->exportMemory(handle, error)) { destroy(); return false; }

    // GL import. GL_EXT_memory_object: "DEDICATED_MEMORY_OBJECT_EXT ... must be set to TRUE when the external memory
    // handle ... was created as a dedicated allocation"; parameters "become immutable once the object is associated with
    // external memory", so it is set before the import.
    glErrors();
    sGL.CreateMemoryObjectsEXT(1, &mGlMemory);
    const GLint dedicated = GL_TRUE;
    sGL.MemoryObjectParameterivEXT(mGlMemory, WOLF_GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
    std::string e = glErrors();
    if (!e.empty()) { error = "glMemoryObjectParameterivEXT: " + e; device.closeExternalHandle(handle); destroy(); return false; }
    // "A successful import operation transfers ownership of <fd> to the GL implementation" (GL_EXT_memory_object_fd);
    // likewise the win32 handle (GL_EXT_memory_object_win32, OPAQUE_WIN32 handles)
#if LL_WINDOWS
    sGL.ImportMemoryWin32HandleEXT(mGlMemory, static_cast<GLuint64>(mImage->allocationSize()), WOLF_GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle.mWin32);
#else
    sGL.ImportMemoryFdEXT(mGlMemory, static_cast<GLuint64>(mImage->allocationSize()), WOLF_GL_HANDLE_TYPE_OPAQUE_FD_EXT, handle.mFd);
#endif
    e = glErrors();
    if (!e.empty()) { error = "glImportMemoryFdEXT: " + e; device.closeExternalHandle(handle); destroy(); return false; }
    // GL owns the handle now

    GLint saved_tex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved_tex);
    glGenTextures(1, &mGlTexture);
    glBindTexture(GL_TEXTURE_2D, mGlTexture);
    // "TEXTURE_TILING_EXT ... takes effect the next time storage is allocated from a memory object"; must match the Vulkan tiling
    glTexParameteri(GL_TEXTURE_2D, WOLF_GL_TEXTURE_TILING_EXT, static_cast<GLint>(WOLF_GL_OPTIMAL_TILING_EXT));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    // immutable storage over the imported memory at offset 0 (the allocation is the image's own)
    sGL.TexStorageMem2DEXT(GL_TEXTURE_2D, 1, static_cast<GLenum>(gl_internal_format), static_cast<GLsizei>(width), static_cast<GLsizei>(height), mGlMemory, 0);
    e = glErrors();
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(saved_tex));
    if (!e.empty()) { error = llformat("glTexStorageMem2DEXT(0x%x): ", gl_internal_format) + e; destroy(); return false; }
    return true;
}

void WolfVkSharedImage::destroy()
{
    if (gGLManager.mInited)
    {
        if (mGlTexture) glDeleteTextures(1, &mGlTexture);
        if (mGlMemory && sGL.mComplete) sGL.DeleteMemoryObjectsEXT(1, &mGlMemory);
    }
    mGlTexture = mGlMemory = 0;
    mImage.reset();   // its destroy() defers the Vulkan release
    mDevice = nullptr;
}

bool WolfVkShare::loadGL()
{
    loadGLFunctions();
    return sGL.mComplete;
}

// ---- layers --------------------------------------------------------------------------------------------------------

bool WolfVkShare::createLayer(Layer& layer, uint32_t width, uint32_t height, bool depth, std::string& error)
{
    layer.mDepth = depth;
    const WolfVkImage::Format format = depth ? WolfVkImage::formatFor(GL_DEPTH_COMPONENT32F, GL_FLOAT) : WolfVkImage::formatFor(GL_RGBA8, GL_UNSIGNED_BYTE);
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                                  | (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (!layer.mShared.create(*mDevice, format, width, height, usage, depth ? GL_DEPTH_COMPONENT32F : GL_RGBA8, error))
    {
        return false;
    }
    layer.mImage = layer.mShared.image();

    // a framebuffer with it attached, for the blits and the depth pass
    glErrors();
    GLint saved_draw = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &saved_draw);
    glGenFramebuffers(1, &layer.mGlFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, layer.mGlFbo);
    if (depth)
    {
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, layer.glTexture(), 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
    }
    else
    {
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, layer.glTexture(), 0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
    }
    const GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    const std::string e = glErrors();
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(saved_draw));
    if (status != GL_FRAMEBUFFER_COMPLETE || !e.empty())
    {
        error = llformat("shared %s layer framebuffer incomplete (status 0x%x) %s", depth ? "depth" : "colour", status, e.c_str());
        return false;
    }
    return true;
}

void WolfVkShare::destroyLayer(Layer& layer)
{
    if (gGLManager.mInited && layer.mGlFbo) glDeleteFramebuffers(1, &layer.mGlFbo);
    layer.mGlFbo = 0;
    layer.mImage = nullptr;
    layer.mShared.destroy();
}

bool WolfVkShare::createSemaphores(std::string& error)
{
    WolfVkDevice::ExternalHandle h1, h2;
    if (!mDevice->createExportedSemaphore(mGlDone, h1, error)) return false;
    if (!mDevice->createExportedSemaphore(mVkDone, h2, error)) { mDevice->closeExternalHandle(h1); return false; }
    glErrors();
    GLuint sems[2] = { 0, 0 };
    sGL.GenSemaphoresEXT(2, sems);
    mGlSemGlDone = sems[0];
    mGlSemVkDone = sems[1];
    // "glImportSemaphoreFdEXT(semaphore, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd)": fd ownership passes to GL on success
#if LL_WINDOWS
    sGL.ImportSemaphoreWin32HandleEXT(mGlSemGlDone, WOLF_GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, h1.mWin32);
    sGL.ImportSemaphoreWin32HandleEXT(mGlSemVkDone, WOLF_GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, h2.mWin32);
#else
    sGL.ImportSemaphoreFdEXT(mGlSemGlDone, WOLF_GL_HANDLE_TYPE_OPAQUE_FD_EXT, h1.mFd);
    sGL.ImportSemaphoreFdEXT(mGlSemVkDone, WOLF_GL_HANDLE_TYPE_OPAQUE_FD_EXT, h2.mFd);
#endif
    const std::string e = glErrors();
    if (!e.empty())
    {
        error = "glImportSemaphoreFdEXT: " + e;
        mDevice->closeExternalHandle(h1);
        mDevice->closeExternalHandle(h2);
        return false;
    }
    return true;
}

bool WolfVkShare::createDepthCopyProgram(std::string& error)
{
    glErrors();
    auto compile = [&](GLenum type, const char* src, GLuint& out) -> bool
    {
        out = glCreateShader(type);
        glShaderSource(out, 1, &src, nullptr);
        glCompileShader(out);
        GLint ok = GL_FALSE;
        glGetShaderiv(out, GL_COMPILE_STATUS, &ok);
        if (ok != GL_TRUE)
        {
            char log[1024] = { 0 };
            glGetShaderInfoLog(out, sizeof(log) - 1, nullptr, log);
            error = std::string("depth copy shader: ") + log;
            glDeleteShader(out);
            out = 0;
            return false;
        }
        return true;
    };
    GLuint vs = 0, fs = 0;
    if (!compile(GL_VERTEX_SHADER, sDepthCopyVert, vs)) return false;
    if (!compile(GL_FRAGMENT_SHADER, sDepthCopyFrag, fs)) { glDeleteShader(vs); return false; }
    mDepthProgram = glCreateProgram();
    glAttachShader(mDepthProgram, vs);
    glAttachShader(mDepthProgram, fs);
    glLinkProgram(mDepthProgram);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = GL_FALSE;
    glGetProgramiv(mDepthProgram, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE)
    {
        error = "depth copy program failed to link";
        destroyGLProgram();
        return false;
    }
    mDepthTexLoc = glGetUniformLocation(mDepthProgram, "depthTex");
    mDepthOriginLoc = glGetUniformLocation(mDepthProgram, "origin");
    glGenVertexArrays(1, &mDepthVao);
    const std::string e = glErrors();
    if (!e.empty()) { error = "depth copy program: " + e; destroyGLProgram(); return false; }
    return true;
}

void WolfVkShare::destroyGLProgram()
{
    if (gGLManager.mInited)
    {
        if (mDepthProgram) glDeleteProgram(mDepthProgram);
        if (mDepthVao) glDeleteVertexArrays(1, &mDepthVao);
    }
    mDepthProgram = 0;
    mDepthVao = 0;
    mDepthTexLoc = mDepthOriginLoc = -1;
}

// ---- Vulkan side ----------------------------------------------------------------------------------------------------

void WolfVkShare::imageBarrier(VkCommandBuffer cmd, Layer& layer, VkImageLayout old_layout, VkImageLayout new_layout,
                               VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src_stages, VkPipelineStageFlags dst_stages,
                               uint32_t src_family, uint32_t dst_family)
{
    if (!layer.mImage || !layer.mImage->isValid()) return;
    // Source: vulkan_core.h:4025 VkImageMemoryBarrier { sType, pNext, srcAccessMask, dstAccessMask, oldLayout, newLayout,
    // srcQueueFamilyIndex, dstQueueFamilyIndex, image, subresourceRange }; :4596 PFN_vkCmdPipelineBarrier
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = src_family;
    b.dstQueueFamilyIndex = dst_family;
    b.image = layer.mImage->image();
    b.subresourceRange = { layer.mImage->aspect(), 0, 1, 0, 1 };
    gVk.vkCmdPipelineBarrier(cmd, src_stages, dst_stages, 0, 0, nullptr, 0, nullptr, 1, &b);
    layer.mImage->noteLayout(new_layout, dst_stages, dst_access);
}

bool WolfVkShare::primeVulkan(std::string& error)
{
    // Fresh images: UNDEFINED -> the hand layouts, released to the external queue family, and one vk_done signal so GL's
    // first wait has a signal to consume. Spec (external memory): "Resources shared with external APIs ... must
    // explicitly manage ownership transfers between local and external queues" - VK_QUEUE_FAMILY_EXTERNAL (:5477).
    VkCommandBuffer cmd = mDevice->beginOneTime();
    if (cmd == VK_NULL_HANDLE) { error = "no one-time command buffer"; return false; }
    const uint32_t ours = mDevice->queueFamily();
    for (Layer* l : { &mWorld, &mUI })
    {
        imageBarrier(cmd, *l, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                     VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ours, VK_QUEUE_FAMILY_EXTERNAL);
    }
    imageBarrier(cmd, mDepth, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0,
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ours, VK_QUEUE_FAMILY_EXTERNAL);
    WolfVkDevice::ExtraSync sync;
    if (!mVkSignalled)
    {
        sync.mSignals.push_back(mVkDone);
        mVkSignalled = true;
    }
    return mDevice->submitSync(cmd, sync, true, error);
}

WolfVkDevice::ExtraSync WolfVkShare::sync()
{
    WolfVkDevice::ExtraSync s;
    if (mGlSignalled)
    {
        s.mWaits.push_back(mGlDone);
        // the layers are first read by the scene pass's depth test and the composite's fragment shader
        s.mWaitStages.push_back(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        mGlSignalled = false;
    }
    if (!mVkSignalled)
    {
        s.mSignals.push_back(mVkDone);
        mVkSignalled = true;
    }
    return s;
}

void WolfVkShare::acquire(VkCommandBuffer cmd)
{
    const uint32_t ours = mDevice->queueFamily();
    for (Layer* l : { &mWorld, &mUI })
    {
        imageBarrier(cmd, *l, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_QUEUE_FAMILY_EXTERNAL, ours);
    }
    imageBarrier(cmd, mDepth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                 VK_QUEUE_FAMILY_EXTERNAL, ours);
}

void WolfVkShare::release(VkCommandBuffer cmd)
{
    const uint32_t ours = mDevice->queueFamily();
    for (Layer* l : { &mWorld, &mUI })
    {
        imageBarrier(cmd, *l, l->mImage ? l->mImage->layout() : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_ACCESS_SHADER_READ_BIT, 0,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ours, VK_QUEUE_FAMILY_EXTERNAL);
    }
    // [BLUE DASHES 2026-11-08] both fragment-test stages write depth (vk.xml syncaccess VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)
    imageBarrier(cmd, mDepth, mDepth.mImage ? mDepth.mImage->layout() : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0,
                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ours, VK_QUEUE_FAMILY_EXTERNAL);
}

// ---- GL side ---------------------------------------------------------------------------------------------------------

void WolfVkShare::glHandLayouts(uint32_t textures[3], uint32_t layouts[3]) const
{
    textures[0] = mWorld.glTexture(); layouts[0] = WOLF_GL_LAYOUT_COLOR_ATTACHMENT_EXT;
    textures[1] = mUI.glTexture();    layouts[1] = WOLF_GL_LAYOUT_COLOR_ATTACHMENT_EXT;
    textures[2] = mDepth.glTexture(); layouts[2] = WOLF_GL_LAYOUT_DEPTH_STENCIL_ATTACHMENT_EXT;
}

bool WolfVkShare::glCaptureWorld(uint32_t gl_depth_texture, uint32_t depth_width, uint32_t depth_height, const LLRect& world_rect)
{
    if (!isValid() || broken()) return false;
    if (!mWorldRect) mWorldRect.reset(new LLRect(world_rect)); else *mWorldRect = world_rect;
    GLStateKeeper keep;
    glErrors();
    // Vulkan's last use of the layers must be over before GL writes them. GL_EXT_semaphore: "WaitSemaphoreEXT ... the
    // current layout of the textures must be specified to initialize internal GL state"; GL then owns the images.
    if (mVkSignalled)
    {
        GLuint textures[3]; GLenum layouts[3];
        glHandLayouts(textures, layouts);
        sGL.WaitSemaphoreEXT(mGlSemVkDone, 0, nullptr, 3, textures, layouts);
        mVkSignalled = false;
        // [HAND-OVER 2026-11-08 8.2k] checked on its own: a wait that raised an error did not happen, the semaphore stays
        // signalled, and Vulkan's next signal of it would be a second signal of a signalled binary semaphore
        const std::string we = glErrors();
        if (!we.empty())
        {
            mBrokenReason = "glWaitSemaphoreEXT (vk_done): " + we;
            LL_WARNS("Vulkan") << "shared layers: " << mBrokenReason << LL_ENDL;
            return false;
        }
    }
    // the finished world: default framebuffer (renderFinalize's last pass drew into its back buffer) -> world layer
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mWorld.mGlFbo);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlitFramebuffer(0, 0, static_cast<GLint>(mWidth), static_cast<GLint>(mHeight), 0, 0, static_cast<GLint>(mWidth), static_cast<GLint>(mHeight), GL_COLOR_BUFFER_BIT, GL_NEAREST);

    // the depth: deferredScreen's GL_DEPTH_COMPONENT24 into the D32F layer at the world view rect (a blit cannot change
    // depth formats, so a full-screen pass writes gl_FragDepth), elsewhere depth 1 (nothing)
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mDepth.mGlFbo);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    glDepthMask(GL_TRUE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glViewport(0, 0, static_cast<GLsizei>(mWidth), static_cast<GLsizei>(mHeight));
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);
    if (gl_depth_texture && depth_width && depth_height && mDepthProgram)
    {
        const GLint x = world_rect.mLeft, y = world_rect.mBottom;
        const GLsizei w = std::min<GLsizei>(static_cast<GLsizei>(depth_width), world_rect.getWidth());
        const GLsizei h = std::min<GLsizei>(static_cast<GLsizei>(depth_height), world_rect.getHeight());
        glViewport(x, y, w, h);
        glUseProgram(mDepthProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, gl_depth_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);   // texelFetch of the depth value itself
        glUniform1i(mDepthTexLoc, 0);
        glUniform2i(mDepthOriginLoc, x, y);
        glBindVertexArray(mDepthVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    const std::string e = glErrors();
    if (!e.empty())
    {
        LL_WARNS_ONCE("Vulkan") << "shared layers, world capture: " << e << LL_ENDL;
    }
    return true;
}

bool WolfVkShare::glCaptureUIAndSignal()
{
    if (!isValid() || broken()) return false;
    GLStateKeeper keep;
    glErrors();
    // a frame without a world capture (login screen) still writes the UI layer: Vulkan's last read must be over
    if (mVkSignalled)
    {
        GLuint textures[3]; GLenum layouts[3];
        glHandLayouts(textures, layouts);
        sGL.WaitSemaphoreEXT(mGlSemVkDone, 0, nullptr, 3, textures, layouts);
        mVkSignalled = false;
        const std::string we = glErrors();   // as in glCaptureWorld
        if (!we.empty())
        {
            mBrokenReason = "glWaitSemaphoreEXT (vk_done): " + we;
            LL_WARNS("Vulkan") << "shared layers: " << mBrokenReason << LL_ENDL;
            return false;
        }
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mUI.mGlFbo);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlitFramebuffer(0, 0, static_cast<GLint>(mWidth), static_cast<GLint>(mHeight), 0, 0, static_cast<GLint>(mWidth), static_cast<GLint>(mHeight), GL_COLOR_BUFFER_BIT, GL_NEAREST);
    // GL_EXT_semaphore: "SignalSemaphoreEXT ... transitions textures to dstLayout and flushes the command stream as if
    // Flush were inserted immediately after the semaphore operation"; the layers belong to Vulkan from here.
    const std::string be = glErrors();
    if (!be.empty())
    {
        LL_WARNS_ONCE("Vulkan") << "shared layers, UI capture: " << be << LL_ENDL;
    }
    GLuint textures[3]; GLenum layouts[3];
    glHandLayouts(textures, layouts);
    sGL.SignalSemaphoreEXT(mGlSemGlDone, 0, nullptr, 3, textures, layouts);
    // [HAND-OVER 2026-11-08 8.2k] mGlSignalled is what makes the frame's Vulkan submit wait on gl_done (sync()); it was set
    // even when the signal raised a GL error. A Vulkan wait on a binary semaphore with no signal submitted for it is
    // invalid (VUID-vkQueueSubmit-pWaitSemaphores-03238) and has nothing that would complete it; the queue, and GL's own
    // wait on vk_done behind it, would then stall - a picture consistent with the Xid 109 of 8.2j, not proven to be its
    // cause. So the flag is set only for a signal that GL accepted.
    const std::string e = glErrors();
    if (!e.empty())
    {
        mBrokenReason = "glSignalSemaphoreEXT (gl_done): " + e;
        LL_WARNS("Vulkan") << "shared layers: " << mBrokenReason << LL_ENDL;
        return false;
    }
    mGlSignalled = true;
    return true;
}

const LLRect& WolfVkShare::worldRect() const
{
    static const LLRect empty;
    return mWorldRect ? *mWorldRect : empty;
}
