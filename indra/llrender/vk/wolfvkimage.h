/**
 * @file wolfvkimage.h
 * @brief Textures (the LLImageGL equivalent), the GL -> Vulkan format table, samplers mirroring LLTexUnit's states.
 *
 * [VULKAN BACKEND 2026-11-08]
 *
 * Format table (WolfVkImage::formatFor): every GL internal format LLImageGL hands to glTexImage2D
 * (llimagegl.cpp:305-354 dataFormatBits lists them; createGLTexture :1593-1615 picks LUMINANCE8 / LUMINANCE8_ALPHA8 /
 * RGB8 / RGBA8 by component count; setManualImage :1333-1356 turns GL_ALPHA, GL_LUMINANCE and GL_LUMINANCE_ALPHA into
 * R8 / R8 / RG8 with a texture swizzle on the core profile, :1421-1468 maps the generic GL_COMPRESSED_* on request;
 * the render targets use GL_RGBA, GL_RGBA8, GL_RGB, GL_RGB8, GL_RGBA16F, GL_RGBA32F, GL_R16F, GL_RG16F, GL_R8 and
 * depth GL_DEPTH_COMPONENT24, llrendertarget.cpp:98/307) mapped to a VkFormat with the same channels and precision:
 *
 *   GL                                 VkFormat (vulkan_core.h)                        note
 *   GL_RGBA, GL_RGBA8                  VK_FORMAT_R8G8B8A8_UNORM (:1896)
 *   GL_SRGB_ALPHA, GL_SRGB8_ALPHA8     VK_FORMAT_R8G8B8A8_SRGB (:1902)
 *   GL_RGB, GL_RGB8                    VK_FORMAT_R8G8B8A8_UNORM, rows expanded RGB->RGBA (alpha 255): three-channel
 *                                      8-bit images are optional in Vulkan (spec 34.3 "Required Format Support" lists
 *                                      only R8G8B8A8 of the two for sampling), so the data is widened on upload
 *   GL_SRGB, GL_SRGB8                  VK_FORMAT_R8G8B8A8_SRGB, expanded likewise
 *   GL_LUMINANCE, GL_LUMINANCE8        VK_FORMAT_R8_UNORM (:1868), view swizzle (R,R,R,ONE)   = llimagegl.cpp:1341-1347
 *   GL_LUMINANCE_ALPHA, GL_LUMINANCE8_ALPHA8  VK_FORMAT_R8G8_UNORM (:1875), swizzle (R,R,R,G) = :1349-1355
 *   GL_ALPHA, GL_ALPHA8                VK_FORMAT_R8_UNORM, swizzle (ZERO,ZERO,ZERO,R)         = :1333-1339
 *   GL_RED, GL_R8                      VK_FORMAT_R8_UNORM
 *   GL_RG, GL_RG8                      VK_FORMAT_R8G8_UNORM
 *   GL_BGRA                            VK_FORMAT_B8G8R8A8_UNORM (:1903)
 *   GL_RGB10_A2                        VK_FORMAT_A2B10G10R10_UNORM_PACK32 (:1923)
 *   GL_R16F                            VK_FORMAT_R16_SFLOAT (:1935)
 *   GL_RG16F                           VK_FORMAT_R16G16_SFLOAT (:1942)
 *   GL_RGB16F                          VK_FORMAT_R16G16B16A16_SFLOAT (:1956), expanded (R16G16B16 is optional too)
 *   GL_RGBA16F                         VK_FORMAT_R16G16B16A16_SFLOAT (:1956)
 *   GL_R32F                            VK_FORMAT_R32_SFLOAT (:1959)
 *   GL_RGBA32F                         VK_FORMAT_R32G32B32A32_SFLOAT (:1968)
 *   GL_DEPTH_COMPONENT, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT32F   VK_FORMAT_D32_SFLOAT (:1985): the vkglshare probe
 *                                      (2026-11-08) found RADV offers no D24 format at all, and D32_SFLOAT is the
 *                                      depth format the spec requires every device to support for attachments
 *   GL_DEPTH24_STENCIL8, GL_DEPTH32F_STENCIL8   VK_FORMAT_D32_SFLOAT_S8_UINT (:1989) (one of the two mandatory
 *                                      depth/stencil formats; D24S8 is the other and RADV lacks it)
 *   GL_COMPRESSED_RGBA_S3TC_DXT1_EXT   VK_FORMAT_BC1_RGBA_UNORM_BLOCK (:1992); _SRGB_ALPHA_ variant -> BC1_RGBA_SRGB_BLOCK (:1993)
 *   GL_COMPRESSED_RGBA_S3TC_DXT3_EXT   VK_FORMAT_BC2_UNORM_BLOCK (:1994) / BC2_SRGB_BLOCK (:1995)
 *   GL_COMPRESSED_RGBA_S3TC_DXT5_EXT   VK_FORMAT_BC3_UNORM_BLOCK (:1996) / BC3_SRGB_BLOCK (:1997)
 *   GL_COMPRESSED_RED/RG/RGB/RGBA/SRGB/SRGB_ALPHA/LUMINANCE/LUMINANCE_ALPHA/ALPHA   the uncompressed format of the
 *                                      same channels: these ask the GL driver to pick a compression itself
 *                                      (RenderCompressTextures); Vulkan has no such request, so the texture stays
 *                                      uncompressed
 *
 * Upload: the client rows arrive as glTexImage2D would see them, GL_UNPACK_ALIGNMENT at its default 4 (llimagegl.cpp
 * never changes it; it sets GL_UNPACK_SWAP_BYTES :811 and GL_UNPACK_ROW_LENGTH :1195 only), so rows are re-packed
 * tightly into the staging buffer, swapped when the image asked for swapping, and copied with vkCmdCopyBufferToImage
 * (one VkBufferImageCopy per mip level). Images shared with OpenGL later (stage 4 hand-over) must be optimally
 * tiled and dedicated (vkglshare probe); every image here is created with VK_IMAGE_TILING_OPTIMAL, and
 * create(..., dedicated = true) asks the allocator for its own allocation.
 *
 * Samplers (WolfVkSamplerCache): LLTexUnit::setTextureFilteringOption (llrender.cpp:495-553) and
 * setTextureAddressMode (:474-493, sGLAddressMode :94-99 REPEAT / MIRRORED_REPEAT / CLAMP_TO_EDGE) mapped:
 *   TFO_POINT      mag NEAREST, min NEAREST (NEAREST_MIPMAP_NEAREST with mips)          -> VK_FILTER_NEAREST x2, MIPMAP_MODE_NEAREST
 *   TFO_BILINEAR   mag LINEAR, min LINEAR (LINEAR_MIPMAP_NEAREST with mips)             -> VK_FILTER_LINEAR x2, MIPMAP_MODE_NEAREST
 *   TFO_TRILINEAR  mag LINEAR, min LINEAR_MIPMAP_LINEAR                                 -> VK_FILTER_LINEAR x2, MIPMAP_MODE_LINEAR
 *   TFO_ANISOTROPIC  as trilinear + GL_TEXTURE_MAX_ANISOTROPY gGLManager.mMaxAnisotropy -> anisotropyEnable, maxAnisotropy
 *   (llrender.cpp:506-551; VkFilter vulkan_core.h:2372, VkSamplerMipmapMode :2403, VkSamplerAddressMode :2380)
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

#ifndef WOLF_VK_IMAGE_H
#define WOLF_VK_IMAGE_H

#include "wolfvkallocator.h"
#include "wolfvkdevice.h"
#include <map>
#include <string>
#include <vector>

class WolfVkDevice;
class WolfVkBuffer;

class WolfVkImage
{
public:
    // How a GL internal format maps (see the table above).
    struct Format
    {
        VkFormat mFormat = VK_FORMAT_UNDEFINED;
        uint32_t mTexelBytes = 0;        // bytes per texel after any expansion (0 for block formats)
        uint32_t mBlockBytes = 0;        // bytes per 4x4 block for BC formats (8 or 16), 0 otherwise
        uint32_t mSourceChannels = 0;    // channels in the client data (3 for GL_RGB that gets expanded to 4)
        uint32_t mSourceBytesPerChannel = 1;
        bool mExpandRGB = false;         // 3-channel source widened to 4 on upload
        bool mDepth = false;
        bool mStencil = false;
        VkComponentMapping mSwizzle = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
        bool valid() const { return mFormat != VK_FORMAT_UNDEFINED; }
    };
    // gl_internal_format: GL_RGBA8 etc.; gl_pixel_type: GL_UNSIGNED_BYTE, GL_HALF_FLOAT, GL_FLOAT, GL_UNSIGNED_SHORT, GL_UNSIGNED_INT,
    // GL_UNSIGNED_INT_8_8_8_8_REV (the client data's type, llimagegl.cpp:1041-1060). Invalid Format for an unknown pair.
    static Format formatFor(uint32_t gl_internal_format, uint32_t gl_pixel_type);
    static const char* formatName(VkFormat format);

    enum class Kind { TEXTURE_2D, TEXTURE_CUBE, TEXTURE_2D_ARRAY, TEXTURE_3D };

    WolfVkImage() = default;
    ~WolfVkImage();
    WolfVkImage(const WolfVkImage&) = delete;
    WolfVkImage& operator=(const WolfVkImage&) = delete;

    // usage: VK_IMAGE_USAGE_* (SAMPLED is always added; TRANSFER_DST for uploads, TRANSFER_SRC for read-back/mips).
    // layers: 6 for a cube, N for an array, depth for a 3D image (then it is the depth extent).
    bool create(WolfVkDevice& device, Kind kind, const Format& format, uint32_t width, uint32_t height, uint32_t layers,
                uint32_t mip_levels, VkImageUsageFlags usage, bool dedicated, std::string& error);
    // [SHARED IMAGES 2026-11-08] A 2D image whose memory OpenGL will import (wolfvkshare.cpp): VkExternalMemoryImageCreateInfo
    // (vulkan_core.h:5951) with the platform handle type, optimal tiling, one dedicated exportable allocation. The
    // format/usage must be exportable: externalFormatSupported() first.
    bool createExternal(WolfVkDevice& device, const Format& format, uint32_t width, uint32_t height, VkImageUsageFlags usage, std::string& error);
    // vkGetPhysicalDeviceImageFormatProperties2 (:6181) with VkPhysicalDeviceExternalImageFormatInfo (:5915): true when the
    // external memory for this format/tiling/usage can be exported (VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT :5610).
    static bool externalFormatSupported(WolfVkDevice& device, const Format& format, VkImageUsageFlags usage, bool& dedicated_only, std::string& error);
    // The handle of the image's memory for the GL import (the receiver owns it on success), and the allocation's size
    // (glImportMemoryFdEXT / glImportMemoryWin32HandleEXT take the memory object's full size).
    bool exportMemory(WolfVkDevice::ExternalHandle& out, std::string& error);
    VkDeviceSize allocationSize() const { return mAllocation.mSize; }
    void destroy();
    bool isValid() const { return mImage != VK_NULL_HANDLE; }

    // Client data for one mip level (and layer), in GL row layout (alignment 4), width x height of that level.
    // gl_pixel_type as given to formatFor; swap_bytes = LLImageGL::mFormatSwapBytes. Records the copy on cmd, or on
    // a one-time buffer when cmd is null (then waits). The staging memory is freed frames-in-flight later.
    bool upload(VkCommandBuffer cmd, uint32_t mip, uint32_t layer, uint32_t width, uint32_t height, const void* data,
                uint32_t gl_pixel_type, bool swap_bytes, std::string& error);
    // Builds mip levels 1..n-1 from level 0 with vkCmdBlitImage (linear), as glGenerateMipmap does (llimagegl.cpp:876).
    void generateMipmaps(VkCommandBuffer cmd);
    // Transitions the whole image to layout for the given stages; records the barrier on cmd.
    void transition(VkCommandBuffer cmd, VkImageLayout new_layout, VkPipelineStageFlags dst_stages, VkAccessFlags dst_access);
    VkImageLayout layout() const { return mLayout; }
    // Records a layout change that a render pass performed (its finalLayout), without a barrier.
    void noteLayout(VkImageLayout layout, VkPipelineStageFlags stages, VkAccessFlags access) { mLayout = layout; mLastStages = stages; mLastAccess = access; }
    // Copies mip 0 of layer 0 into out (host readable, tightly packed, format texel bytes), waiting for the GPU.
    bool readBack(std::vector<uint8_t>& out, std::string& error);

    VkImage image() const { return mImage; }
    VkImageView view() const { return mView; }
    VkImageUsageFlags usage() const { return mUsage; }
    const Format& format() const { return mFormat; }
    uint32_t width() const { return mWidth; }
    uint32_t height() const { return mHeight; }
    uint32_t layers() const { return mLayers; }
    uint32_t mipLevels() const { return mMipLevels; }
    Kind kind() const { return mKind; }
    VkImageAspectFlags aspect() const;

private:
    bool createInternal(WolfVkDevice& device, Kind kind, const Format& format, uint32_t width, uint32_t height, uint32_t layers,
                        uint32_t mip_levels, VkImageUsageFlags usage, bool dedicated, VkExternalMemoryHandleTypeFlags external, std::string& error);
    WolfVkDevice* mDevice = nullptr;
    Kind mKind = Kind::TEXTURE_2D;
    Format mFormat;
    uint32_t mWidth = 0, mHeight = 0, mLayers = 1, mMipLevels = 1;
    VkImage mImage = VK_NULL_HANDLE;
    VkImageView mView = VK_NULL_HANDLE;
    VkImageUsageFlags mUsage = 0;
    WolfVkAllocation mAllocation;
    VkImageLayout mLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags mLastStages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkAccessFlags mLastAccess = 0;
};

class WolfVkSamplerCache
{
public:
    // LLTexUnit::eTextureFilterOptions / eTextureAddressMode values (llrender.h:80-93), repeated to avoid the include
    enum Filter : uint32_t { TFO_POINT = 0, TFO_BILINEAR, TFO_TRILINEAR, TFO_ANISOTROPIC };
    enum Address : uint32_t { TAM_WRAP = 0, TAM_MIRROR, TAM_CLAMP };

    void init(WolfVkDevice& device, float max_anisotropy) { mDevice = &device; mMaxAnisotropy = max_anisotropy; }
    void destroy();
    // has_mips: the texture has more than one level (the GL min filter differs, llrender.cpp:513-540)
    VkSampler get(Filter filter, Address address, bool has_mips, bool shadow_compare = false);

private:
    WolfVkDevice* mDevice = nullptr;
    float mMaxAnisotropy = 1.0f;
    std::map<uint32_t, VkSampler> mSamplers;
};

#endif // WOLF_VK_IMAGE_H
