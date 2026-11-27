/**
 * @file llimagegl.h
 * @brief Object for managing images and their textures
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


#ifndef LL_LLIMAGEGL_H
#define LL_LLIMAGEGL_H

#include "llimage.h"

#include "llgltypes.h"
#include "llpointer.h"
#include "llrefcount.h"
#include "v2math.h"
#include "llunits.h"
#include "llthreadsafequeue.h"
#include "llrender.h"
#include "threadpool.h"
#include "workqueue.h"
#include <unordered_set>

#define LL_IMAGEGL_THREAD_CHECK 0 //set to 1 to enable thread debugging for ImageGL

class LLWindow;

#define BYTES_TO_MEGA_BYTES(x) ((x) >> 20)
#define MEGA_BYTES_TO_BYTES(x) ((x) << 20)

namespace LLImageGLMemory
{
    void alloc_tex_image(U32 width, U32 height, U32 intformat, U32 count);
    void free_tex_image(U32 texName);
    void free_tex_images(U32 count, const U32* texNames);
    void free_cur_tex_image();
}

//============================================================================
class LLImageGL : public LLRefCount
{
    friend class LLTexUnit;
public:

    // call once per frame
    static void updateClass();

    // Get an estimate of how many bytes have been allocated in vram for textures.
    // Does not include mipmaps.
    // NOTE: multiplying this number by two gives a good estimate for total
    // video memory usage based on testing in lagland against an NVIDIA GPU.
    static U64 getTextureBytesAllocated();

    // These 2 functions replace glGenTextures() and glDeleteTextures()
    static void generateTextures(S32 numTextures, U32 *textures);
    static void deleteTextures(S32 numTextures, const U32 *textures);
    // [VULKAN BACKEND 2026-11-08] The Vulkan backend keeps copies of the textures it draws (newview/wolfvkmirror.cpp): told when
    // a texture's texels changed (setImage, setSubImage, createGLTexture, scaleDown, setSubImageFromFrameBuffer) and when a GL
    // name is deleted. Null when the OpenGL renderer runs alone.
    static void (*sWolfVkOnTexelsChanged)(LLGLuint tex_name);
    static void (*sWolfVkOnDelete)(LLGLuint tex_name);
    void wolfVkTexelsChanged(LLGLuint name = 0) const { if (sWolfVkOnTexelsChanged) { sWolfVkOnTexelsChanged(name ? name : mTexName); } }

    // [VK S6 2026-11-26] Resource ownership, textures held by a raw OpenGL name (the Wolf sky tables, the terrain layer
    // arrays, the cloud maps, LLCubeMapArray ...): when an owner is installed (newview/wolfvktextures.cpp) the texture's
    // memory is a Vulkan image that OpenGL imports (GL_EXT_memory_object), so both renderers use ONE copy and the
    // Vulkan passes find the image by the name (WolfVkTextures::find).
    struct WolfVkTextureOwner
    {
        virtual ~WolfVkTextureOwner() = default;
        // A texture of target (GL_TEXTURE_2D, GL_TEXTURE_CUBE_MAP, GL_TEXTURE_2D_ARRAY, GL_TEXTURE_CUBE_MAP_ARRAY,
        // GL_TEXTURE_3D) with IMMUTABLE storage of levels mip levels in internal_format (a sized format), width x height
        // and, for the array and 3D targets, depth (layers; layer-faces for a cube map array; slices). gl_renders: OpenGL
        // attaches it to a framebuffer, copies a framebuffer into it or runs glGenerateMipmap on it (a driver may make
        // the mips by rendering), so the image must be one that can be rendered into. Returns the OpenGL name, not bound anywhere, or 0
        // when it cannot be made that way (the caller then makes an ordinary OpenGL texture, as it did before).
        // GL_RGB8 / GL_RGB16F have no exportable Vulkan format: the storage is then GL_RGBA8 / GL_RGBA16F with
        // GL_TEXTURE_SWIZZLE_A = GL_ONE, which samples exactly as the three-channel format does.
        virtual U32 createStorage(U32 target, S32 levels, U32 internal_format, S32 width, S32 height, S32 depth, bool gl_renders) = 0;
        // name came from createStorage: it is deleted (the OpenGL texture now, the Vulkan image when no frame in
        // flight uses it) and true is returned. Otherwise false and nothing is done.
        virtual bool destroyStorage(U32 name) = 0;
        // name came from createStorage: what it was made as (the format ASKED for) and true. Otherwise false.
        virtual bool describe(U32 name, U32& internal_format, S32& width, S32& height, S32& depth, S32& levels) = 0;
    };
    static WolfVkTextureOwner* sWolfVkTextureOwner;

    // [NATIVE VULKAN 2026-11-26] Pure-Vulkan texture store installed when gGLManager.mIsNativeVulkan is true.
    // Owns synthetic non-zero texture names and WolfVkImage instances without any OpenGL calls.
    struct WolfVkNativeTextureStore
    {
        virtual ~WolfVkNativeTextureStore() = default;
        virtual U32 generateName() = 0;
        virtual void destroyNames(S32 count, const U32* names) = 0;
        virtual bool uploadImage2D(U32 name, S32 miplevel, U32 intformat, S32 width, S32 height,
                                   U32 pixformat, U32 pixtype, const void* pixels, bool swap_bytes,
                                   bool generate_mips, S32 exact_mips = 0) = 0;
        virtual bool uploadSubImage2D(U32 name, S32 miplevel, S32 x_pos, S32 y_pos, S32 width, S32 height,
                                      S32 row_pixels, U32 intformat, U32 pixformat, U32 pixtype,
                                      const void* pixels, bool swap_bytes) = 0;
        virtual bool generateMipmaps(U32 name) = 0;
        virtual void setSamplerOptions(U32 name, S32 filter, S32 address) = 0;
        virtual bool getSamplerOptions(U32 name, S32& filter, S32& address) = 0;
        virtual bool readBackImage2D(U32 name, S32 gl_discard, S32 width, S32 height, S32 ncomponents, U8* out_data) = 0;
    };
    static WolfVkNativeTextureStore* sWolfVkNativeStore;
    static U32 wolfVkCreateStorage(U32 target, S32 levels, U32 internal_format, S32 width, S32 height, S32 depth, bool gl_renders)
    {
        return sWolfVkTextureOwner ? sWolfVkTextureOwner->createStorage(target, levels, internal_format, width, height, depth, gl_renders) : 0;
    }
    // For the callers that delete their names with glDeleteTextures themselves (deleteTextures() below asks the owner
    // by itself): true when the owner held name and deleted it.
    static bool wolfVkDestroyStorage(U32 name) { return name && sWolfVkTextureOwner && sWolfVkTextureOwner->destroyStorage(name); }
    // For the callers that (re)specify the WHOLE of a one-level 2D texture from client data on every upload ("if no
    // name: generateTextures; bind on unit 0; setManualImage(level 0, no compression)"): the same in one call, with the
    // texture in Vulkan-owned memory when an owner is installed. name: the caller's texture name, made here when 0 and
    // REPLACED when the texture is owned and intformat / width / height changed (immutable storage; the old name is
    // deleted like any other). pixformat / pixtype / pixels as for glTexImage2D; pixformat must be one core OpenGL
    // takes as it is (GL_RED, GL_RG, GL_RGB, GL_RGBA: setManualImage rewrites only the deprecated GL_ALPHA /
    // GL_LUMINANCE kinds). The texture is left bound on texture unit 0 (LLTexUnit::bindManual), as those callers had it.
    static void wolfVkManualImage2D(U32& name, U32 intformat, S32 width, S32 height, U32 pixformat, U32 pixtype, const void* pixels);
    // The number of levels of a full mip chain, floor(log2(largest size)) + 1: what glGenerateMipmap fills and the most
    // glTexStorage2D/3D accept (Khronos reference pages glTexStorage2D / glTexStorage3D, GL_INVALID_OPERATION rule).
    // depth counts for GL_TEXTURE_3D only: pass 1 for the array targets, whose layers are not mipped.
    static S32 wolfVkFullMipLevels(S32 width, S32 height, S32 depth = 1)
    {
        S32 size = llmax(width, llmax(height, depth)), levels = 1;
        while (size > 1) { size >>= 1; ++levels; }
        return levels;
    }

    // Size calculation
    static S32 dataFormatBits(S32 dataformat);
    static S64 dataFormatBytes(S32 dataformat, S32 width, S32 height);
    static S32 dataFormatComponents(S32 dataformat);

    bool updateBindStats() const ;
    F32 getTimePassedSinceLastBound();
    void forceUpdateBindStats(void) const;

    // needs to be called every frame
    static void updateStats(F32 current_time);

    // cleanup GL state
    static void destroyGL();
    static void dirtyTexOptions();

    static bool checkSize(S32 width, S32 height);

    //for server side use only.
    // Not currently necessary for LLImageGL, but required in some derived classes,
    // so include for compatability
    static bool create(LLPointer<LLImageGL>& dest, bool usemipmaps = true);
    static bool create(LLPointer<LLImageGL>& dest, U32 width, U32 height, U8 components, bool usemipmaps = true);
    static bool create(LLPointer<LLImageGL>& dest, const LLImageRaw* imageraw, bool usemipmaps = true);

public:
    LLImageGL(bool usemipmaps = true, bool allow_compression = true);
    LLImageGL(U32 width, U32 height, U8 components, bool usemipmaps = true, bool allow_compression = true);
    LLImageGL(const LLImageRaw* imageraw, bool usemipmaps = true, bool allow_compression = true);

    // For wrapping textures created via GL elsewhere with our API only. Use with caution.
    LLImageGL(LLGLuint mTexName, U32 components, LLGLenum target, LLGLint  formatInternal, LLGLenum formatPrimary, LLGLenum formatType, LLTexUnit::eTextureAddressMode addressMode);

protected:
    virtual ~LLImageGL();

    void analyzeAlpha(const void* data_in, U32 w, U32 h);
    void calcAlphaChannelOffsetAndStride();

public:
    virtual void dump();    // debugging info to LL_INFOS()

    bool setSize(S32 width, S32 height, S32 ncomponents, S32 discard_level = -1);
    void setComponents(S32 ncomponents) { mComponents = (S8)ncomponents ;}
    void setAllowCompression(bool allow) { mAllowCompression = allow; }

    static void setManualImage(U32 target, S32 miplevel, S32 intformat, S32 width, S32 height, U32 pixformat, U32 pixtype, const void *pixels, bool allow_compression = true);

    bool createGLTexture() ;
    bool createGLTexture(S32 discard_level, const LLImageRaw* imageraw, S32 usename = 0, bool to_create = true,
        S32 category = sMaxCategories-1, bool defer_copy = false, LLGLuint* tex_name = nullptr);
    bool createGLTexture(S32 discard_level, const U8* data, bool data_hasmips = false, S32 usename = 0, bool defer_copy = false, LLGLuint* tex_name = nullptr);
    void setImage(const LLImageRaw* imageraw);
    bool setImage(const U8* data_in, bool data_hasmips = false, S32 usename = 0);
    // *TODO: This function may not work if the textures is compressed (i.e.
    // RenderCompressTextures is 0). Partial image updates do not work on
    // compressed textures.
    bool setSubImage(const LLImageRaw* imageraw, S32 x_pos, S32 y_pos, S32 width, S32 height, bool force_fast_update = false, LLGLuint use_name = 0);
    bool setSubImage(const U8* datap, S32 data_width, S32 data_height, S32 x_pos, S32 y_pos, S32 width, S32 height, bool force_fast_update = false, LLGLuint use_name = 0);
    bool setSubImageFromFrameBuffer(S32 fb_x, S32 fb_y, S32 x_pos, S32 y_pos, S32 width, S32 height);

    // wait for gl commands to finish on current thread and push
    // a lambda to main thread to swap mNewTexName and mTexName
    void syncToMainThread(LLGLuint new_tex_name);

    // Read back a raw image for this discard level, if it exists
    bool readBackRaw(S32 discard_level, LLImageRaw* imageraw, bool compressed_ok) const;
    void destroyGLTexture();
    void forceToInvalidateGLTexture();

    void setExplicitFormat(LLGLint internal_format, LLGLenum primary_format, LLGLenum type_format = 0, bool swap_bytes = false);
    void setComponents(S8 ncomponents) { mComponents = ncomponents; }

    S32  getDiscardLevel() const        { return mCurrentDiscardLevel; }
    S32  getMaxDiscardLevel() const     { return mMaxDiscardLevel; }

    // override the current discard level
    // should only be used for local textures where you know exactly what you're doing
    void setDiscardLevel(S32 level) { mCurrentDiscardLevel = level; }

    S32  getCurrentWidth() const { return mWidth ;}
    S32  getCurrentHeight() const { return mHeight ;}
    S32  getWidth(S32 discard_level = -1) const;
    S32  getHeight(S32 discard_level = -1) const;
    U8   getComponents() const { return mComponents; }
    S64  getBytes(S32 discard_level = -1) const;
    S64  getMipBytes(S32 discard_level = -1) const;
    bool getBoundRecently() const;
    bool isJustBound() const;
    bool getHasExplicitFormat() const { return mHasExplicitFormat; }
    LLGLenum getPrimaryFormat() const { return mFormatPrimary; }
    LLGLenum getFormatType() const { return mFormatType; }

    bool getHasGLTexture() const { return mTexName != 0; }
    LLGLuint getTexName() const { return mTexName; }

    bool getIsAlphaMask() const;

    bool getIsResident(bool test_now = false); // not const

    void setTarget(const LLGLenum target, const LLTexUnit::eTextureType bind_target);

    LLTexUnit::eTextureType getTarget(void) const { return mBindTarget; }
    bool isGLTextureCreated(void) const { return mGLTextureCreated ; }
    void setGLTextureCreated (bool initialized) { mGLTextureCreated = initialized; }

    bool getUseMipMaps() const { return mUseMipMaps; }
    void setUseMipMaps(bool usemips) { mUseMipMaps = usemips; }
    void setHasMipMaps(bool hasmips) { mHasMipMaps = hasmips; }
    void updatePickMask(S32 width, S32 height, const U8* data_in);
// [RLVa:KB] - Checked: RLVa-2.2 (@setoverlay)
    bool getMask(const LLVector2 &tc) const;
// [/RLVa:KB]
//  bool getMask(const LLVector2 &tc);

    void checkTexSize(bool forced = false) const ;

    // Sets the addressing mode used to sample the texture
    //  (such as wrapping, mirrored wrapping, and clamp)
    // Note: this actually gets set the next time the texture is bound.
    void setAddressMode(LLTexUnit::eTextureAddressMode mode);
    LLTexUnit::eTextureAddressMode getAddressMode(void) const { return mAddressMode; }

    // Sets the filtering options used to sample the texture
    //  (such as point sampling, bilinear interpolation, mipmapping, and anisotropic filtering)
    // Note: this actually gets set the next time the texture is bound.
    void setFilteringOption(LLTexUnit::eTextureFilterOptions option);
    LLTexUnit::eTextureFilterOptions getFilteringOption(void) const { return mFilterOption; }

    LLGLenum getTexTarget()const { return mTarget; }

    void init(bool usemipmaps, bool allow_compression);
    virtual void cleanup(); // Clean up the LLImageGL so it can be reinitialized.  Be careful when using this in derived class destructors

    void setNeedsAlphaAndPickMask(bool need_mask);

#if LL_IMAGEGL_THREAD_CHECK
    // thread debugging
    std::thread::id mActiveThread;
    void checkActiveThread();
#endif

    // scale down to the desired discard level using GPU
    // returns true if texture was scaled down
    // desired discard will be clamped to max discard
    // if desired discard is less than or equal to current discard, no scaling will occur
    // only works for GL_TEXTURE_2D target
    bool scaleDown(S32 desired_discard);

public:
    // Various GL/Rendering options
    S64Bytes mTextureMemory;
    mutable F32  mLastBindTime; // last time this was bound, by discard level

private:
    U32 createPickMask(S32 pWidth, S32 pHeight);
    void freePickMask();
    bool isCompressed();

    LLPointer<LLImageRaw> mSaveData; // used for destroyGL/restoreGL
    LL::WorkQueue::weak_t mMainQueue;
    U8* mPickMask;  //downsampled bitmap approximation of alpha channel.  NULL if no alpha channel
    U16 mPickMaskWidth;
    U16 mPickMaskHeight;
    S8 mUseMipMaps;
    bool mHasExplicitFormat; // If false (default), GL format is f(mComponents)
    bool mAutoGenMips = false;

    bool mIsMask;
    bool mNeedsAlphaAndPickMask;
    S8   mAlphaStride ;
    S8   mAlphaOffset ;

    bool     mGLTextureCreated ;
    LLGLuint mTexName;
    U16      mWidth;
    U16      mHeight;
    S8       mCurrentDiscardLevel;

    bool mAllowCompression;

protected:
    LLGLenum mTarget;       // Normally GL_TEXTURE2D, sometimes something else (ex. cube maps)
    LLTexUnit::eTextureType mBindTarget;    // Normally TT_TEXTURE, sometimes something else (ex. cube maps)
    bool mHasMipMaps;
    S32 mMipLevels;

    LLGLboolean mIsResident;

    S8 mComponents;
    S8 mMaxDiscardLevel;

    bool    mTexOptionsDirty;
    LLTexUnit::eTextureAddressMode      mAddressMode;   // Defaults to TAM_WRAP
    LLTexUnit::eTextureFilterOptions    mFilterOption;  // Defaults to TFO_ANISOTROPIC

    LLGLint  mFormatInternal; // = GL internalformat
    LLGLenum mFormatPrimary;  // = GL format (pixel data format)
    LLGLenum mFormatType;
    bool     mFormatSwapBytes;// if true, use glPixelStorei(GL_UNPACK_SWAP_BYTES, 1)

    bool mExternalTexture;

    // STATICS
public:
    static std::unordered_set<LLImageGL*> sImageList;
    static S32 sCount;
    static U32 sFrameCount;
    static F32 sLastFrameTime;

    // Global memory statistics
    static U32 sBindCount;                  // Tracks number of texture binds for current frame
    static U32 sUniqueCount;                // Tracks number of unique texture binds for current frame
    static bool sGlobalUseAnisotropic;
    static LLImageGL* sDefaultGLTexture ;
    static bool sAutomatedTest;
    static bool sCompressTextures;          //use GL texture compression
#if DEBUG_MISS
    bool mMissed; // Missed on last bind?
    bool getMissed() const { return mMissed; };
#else
    bool getMissed() const { return false; };
#endif

public:
    static void initClass(LLWindow* window, S32 num_catagories, bool skip_analyze_alpha = false, bool thread_texture_loads = false, bool thread_media_updates = false);
    static void allocateConversionBuffer();
    static void cleanupClass() ;

private:
    static S32 sMaxCategories;
    static bool sSkipAnalyzeAlpha;
    static U32 sScratchPBO;
    static U32 sScratchPBOSize;
    static U32* sManualScratch;

    //the flag to allow to call readBackRaw(...).
    //can be removed if we do not use that function at all.
    static bool sAllowReadBackRaw ;
//
//****************************************************************************************************
//The below for texture auditing use only
//****************************************************************************************************
private:
    S32 mCategory ;
public:
    void setCategory(S32 category) {mCategory = category;}
    S32  getCategory()const {return mCategory;}

    void setTexName(GLuint texName) { mTexName = texName; }

    //similar to setTexName, but will call deleteTextures on mTexName if mTexName is not 0 or texname
    void syncTexName(LLGLuint texname);

    //for debug use: show texture size distribution
    //----------------------------------------
    static S32 sCurTexSizeBar ;
    static S32 sCurTexPickSize ;

    static void setCurTexSizebar(S32 index, bool set_pick_size = true) ;
    static void resetCurTexSizebar();

//****************************************************************************************************
//End of definitions for texture auditing use only
//****************************************************************************************************

};

class LLImageGLThread : public LLSimpleton<LLImageGLThread>, LL::ThreadPool
{
public:
    // follows gSavedSettings "RenderGLMultiThreadedTextures"
    static bool sEnabledTextures;
    // follows gSavedSettings "RenderGLMultiThreadedMedia"
    static bool sEnabledMedia;

    LLImageGLThread(LLWindow* window);

    // post a function to be executed on the LLImageGL background thread
    template <typename CALLABLE>
    bool post(CALLABLE&& func)
    {
        return getQueue().post(std::forward<CALLABLE>(func));
    }

    void run() override;

private:
    LLWindow* mWindow;
    void* mContext = nullptr;
    LLAtomicBool mFinished;
};

#endif // LL_LLIMAGEGL_H
