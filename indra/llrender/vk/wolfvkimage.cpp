/**
 * @file wolfvkimage.cpp
 * @brief Textures, the GL -> Vulkan format table and samplers (see wolfvkimage.h for the table and its sources).
 *
 * [VULKAN BACKEND 2026-11-08]
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

#include "linden_common.h"
#include "wolfvkimage.h"
#include "wolfvkdevice.h"
#include "wolfvkbuffer.h"
#include "llglheaders.h"   // the GL_* enumerants the viewer's texture code uses
#include <cstring>
#include <algorithm>

namespace
{
    WolfVkImage::Format simple(VkFormat f, uint32_t texel_bytes, uint32_t src_channels, uint32_t src_bytes_per_channel, bool expand_rgb = false)
    {
        WolfVkImage::Format r;
        r.mFormat = f;
        r.mTexelBytes = texel_bytes;
        r.mSourceChannels = src_channels;
        r.mSourceBytesPerChannel = src_bytes_per_channel;
        r.mExpandRGB = expand_rgb;
        return r;
    }

    WolfVkImage::Format swizzled(VkFormat f, uint32_t texel_bytes, uint32_t src_channels,
                                 VkComponentSwizzle r, VkComponentSwizzle g, VkComponentSwizzle b, VkComponentSwizzle a)
    {
        WolfVkImage::Format res = simple(f, texel_bytes, src_channels, 1);
        res.mSwizzle = { r, g, b, a };
        return res;
    }

    WolfVkImage::Format block(VkFormat f, uint32_t block_bytes)
    {
        WolfVkImage::Format r;
        r.mFormat = f;
        r.mBlockBytes = block_bytes;
        return r;
    }

    WolfVkImage::Format depth(VkFormat f, bool stencil)
    {
        WolfVkImage::Format r;
        r.mFormat = f;
        r.mTexelBytes = stencil ? 8 : 4;   // D32_SFLOAT is 4 bytes; D32_SFLOAT_S8 is written per aspect
        r.mSourceChannels = 1;
        r.mSourceBytesPerChannel = 4;
        r.mDepth = true;
        r.mStencil = stencil;
        return r;
    }

    uint32_t bytesForPixelType(uint32_t gl_pixel_type)
    {
        // Source: llimagegl.cpp:1041-1060 type_width_from_pixtype
        switch (gl_pixel_type)
        {
        case GL_UNSIGNED_BYTE: case GL_BYTE: case GL_UNSIGNED_INT_8_8_8_8_REV: return 1;
        case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: return 2;
        case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: return 4;
        default: return 0;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Format table (wolfvkimage.h)

WolfVkImage::Format WolfVkImage::formatFor(uint32_t gl_internal_format, uint32_t gl_pixel_type)
{
    const uint32_t bpc = bytesForPixelType(gl_pixel_type);
    switch (gl_internal_format)
    {
    case GL_RGBA: case GL_RGBA8: case GL_COMPRESSED_RGBA:
        if (bpc == 2) return simple(VK_FORMAT_R16G16B16A16_SFLOAT, 8, 4, 2);   // GL_RGBA with GL_HALF_FLOAT data (terrain paint, llimagegl.cpp:1051)
        if (bpc == 4) return simple(VK_FORMAT_R32G32B32A32_SFLOAT, 16, 4, 4);
        return simple(VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 1);
    case GL_SRGB_ALPHA: case GL_SRGB8_ALPHA8: case GL_COMPRESSED_SRGB_ALPHA:
        return simple(VK_FORMAT_R8G8B8A8_SRGB, 4, 4, 1);
    case GL_RGB: case GL_RGB8: case GL_COMPRESSED_RGB:
        if (bpc == 2) return simple(VK_FORMAT_R16G16B16A16_SFLOAT, 8, 3, 2, true);
        if (bpc == 4) return simple(VK_FORMAT_R32G32B32A32_SFLOAT, 16, 3, 4, true);
        return simple(VK_FORMAT_R8G8B8A8_UNORM, 4, 3, 1, true);
    case GL_SRGB: case GL_SRGB8: case GL_COMPRESSED_SRGB:
        return simple(VK_FORMAT_R8G8B8A8_SRGB, 4, 3, 1, true);
    case GL_LUMINANCE: case GL_LUMINANCE8: case GL_COMPRESSED_LUMINANCE:
        return swizzled(VK_FORMAT_R8_UNORM, 1, 1, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE);   // :2319, :2318
    case GL_LUMINANCE_ALPHA: case GL_LUMINANCE8_ALPHA8: case GL_COMPRESSED_LUMINANCE_ALPHA:
        return swizzled(VK_FORMAT_R8G8_UNORM, 2, 2, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G);   // :2320
    case GL_ALPHA: case GL_ALPHA8: case GL_COMPRESSED_ALPHA:
        return swizzled(VK_FORMAT_R8_UNORM, 1, 1, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R);   // :2317
    case GL_RED: case GL_R8: case GL_COMPRESSED_RED:
        if (bpc == 2) return simple(VK_FORMAT_R16_SFLOAT, 2, 1, 2);
        if (bpc == 4) return simple(VK_FORMAT_R32_SFLOAT, 4, 1, 4);
        return simple(VK_FORMAT_R8_UNORM, 1, 1, 1);
    case GL_RG: case GL_RG8: case GL_COMPRESSED_RG:
        if (bpc == 2) return simple(VK_FORMAT_R16G16_SFLOAT, 4, 2, 2);
        return simple(VK_FORMAT_R8G8_UNORM, 2, 2, 1);
    case GL_BGRA:
        return simple(VK_FORMAT_B8G8R8A8_UNORM, 4, 4, 1);
    case GL_RGB10_A2:
        return simple(VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, 1, 4);   // one packed 32-bit word per texel
    case GL_R16F:
        return simple(VK_FORMAT_R16_SFLOAT, 2, 1, 2);
    case GL_RG16F:
        return simple(VK_FORMAT_R16G16_SFLOAT, 4, 2, 2);
    case GL_RGB16F:
        return simple(VK_FORMAT_R16G16B16A16_SFLOAT, 8, 3, 2, true);
    case GL_RGBA16F:
        return simple(VK_FORMAT_R16G16B16A16_SFLOAT, 8, 4, 2);
    case GL_RGBA16:   // the G-buffer's normal attachment (pipeline.cpp addDeferredAttachments); 16-bit unsigned normalised
        return simple(VK_FORMAT_R16G16B16A16_UNORM, 8, 4, 2);   // :1950
    case GL_R32F:
        return simple(VK_FORMAT_R32_SFLOAT, 4, 1, 4);
    case GL_RGBA32F:
        return simple(VK_FORMAT_R32G32B32A32_SFLOAT, 16, 4, 4);
    case GL_DEPTH_COMPONENT: case GL_DEPTH_COMPONENT24: case GL_DEPTH_COMPONENT32F:
        return depth(VK_FORMAT_D32_SFLOAT, false);
    case GL_DEPTH24_STENCIL8: case GL_DEPTH32F_STENCIL8:
        return depth(VK_FORMAT_D32_SFLOAT_S8_UINT, true);
    case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT: return block(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8);
    case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT: return block(VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 8);
    case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT: return block(VK_FORMAT_BC2_UNORM_BLOCK, 16);
    case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT: return block(VK_FORMAT_BC2_SRGB_BLOCK, 16);
    case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT: return block(VK_FORMAT_BC3_UNORM_BLOCK, 16);
    case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT: return block(VK_FORMAT_BC3_SRGB_BLOCK, 16);
    default:
        return Format();
    }
}

const char* WolfVkImage::formatName(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
    case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case VK_FORMAT_R8_UNORM: return "R8_UNORM";
    case VK_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_UNORM";
    case VK_FORMAT_R16_SFLOAT: return "R16_SFLOAT";
    case VK_FORMAT_R16G16_SFLOAT: return "R16G16_SFLOAT";
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
    case VK_FORMAT_R32_SFLOAT: return "R32_SFLOAT";
    case VK_FORMAT_R32G32B32A32_SFLOAT: return "R32G32B32A32_SFLOAT";
    case VK_FORMAT_D32_SFLOAT: return "D32_SFLOAT";
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return "D32_SFLOAT_S8_UINT";
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return "BC1_RGBA_UNORM";
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return "BC1_RGBA_SRGB";
    case VK_FORMAT_BC2_UNORM_BLOCK: return "BC2_UNORM";
    case VK_FORMAT_BC2_SRGB_BLOCK: return "BC2_SRGB";
    case VK_FORMAT_BC3_UNORM_BLOCK: return "BC3_UNORM";
    case VK_FORMAT_BC3_SRGB_BLOCK: return "BC3_SRGB";
    default: return "other";
    }
}

// ---------------------------------------------------------------------------------------------------------------
// WolfVkImage

WolfVkImage::~WolfVkImage()
{
    destroy();
}

VkImageAspectFlags WolfVkImage::aspect() const
{
    // Source: vulkan_core.h:2967-2969 VK_IMAGE_ASPECT_COLOR_BIT / DEPTH_BIT / STENCIL_BIT
    if (!mFormat.mDepth) return VK_IMAGE_ASPECT_COLOR_BIT;
    return mFormat.mStencil ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_DEPTH_BIT;
}

bool WolfVkImage::create(WolfVkDevice& device, Kind kind, const Format& format, uint32_t width, uint32_t height, uint32_t layers,
                         uint32_t mip_levels, VkImageUsageFlags usage, bool dedicated, std::string& error)
{
    return createInternal(device, kind, format, width, height, layers, mip_levels, usage, dedicated, 0, error);
}

bool WolfVkImage::createExternal(WolfVkDevice& device, const Format& format, uint32_t width, uint32_t height, VkImageUsageFlags usage, std::string& error)
{
    const VkExternalMemoryHandleTypeFlags types = device.externalHandleType();
    if (types == 0)
    {
        error = "the device cannot export memory";
        return false;
    }
    return createInternal(device, Kind::TEXTURE_2D, format, width, height, 1, 1, usage, true, types, error);
}

bool WolfVkImage::externalFormatSupported(WolfVkDevice& device, const Format& format, VkImageUsageFlags usage, bool& dedicated_only, std::string& error)
{
    dedicated_only = false;
    const VkExternalMemoryHandleTypeFlags types = device.externalHandleType();
    if (types == 0 || !gVk.vkGetPhysicalDeviceImageFormatProperties2)
    {
        error = "no external memory export on this device";
        return false;
    }
    // Source: vulkan_core.h:5915 VkPhysicalDeviceExternalImageFormatInfo { sType, pNext, handleType }; :287 sType;
    // :5827 VkPhysicalDeviceImageFormatInfo2 { sType, pNext, format, type, tiling, usage, flags }; :277 sType;
    // :5921 VkExternalImageFormatProperties { sType, pNext, externalMemoryProperties }; :288 sType;
    // :5821 VkImageFormatProperties2 { sType, pNext, imageFormatProperties }; :276 sType; :6181 the query
    VkPhysicalDeviceExternalImageFormatInfo ext{};
    ext.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
    ext.handleType = static_cast<VkExternalMemoryHandleTypeFlagBits>(types);
    VkPhysicalDeviceImageFormatInfo2 info{};
    info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    info.pNext = &ext;
    info.format = format.mFormat;
    info.type = VK_IMAGE_TYPE_2D;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkExternalImageFormatProperties ext_props{};
    ext_props.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
    VkImageFormatProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    props.pNext = &ext_props;
    VkResult res = gVk.vkGetPhysicalDeviceImageFormatProperties2(device.physical(), &info, &props);
    if (res != VK_SUCCESS)
    {
        error = std::string(formatName(format.mFormat)) + " cannot be an external image here: " + WolfVkLoader::resultString(res);
        return false;
    }
    const VkExternalMemoryFeatureFlags features = ext_props.externalMemoryProperties.externalMemoryFeatures;
    dedicated_only = (features & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0;   // :5609
    if (!(features & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT))                        // :5610
    {
        error = std::string(formatName(format.mFormat)) + ": external memory of this handle type is not exportable";
        return false;
    }
    return true;
}

bool WolfVkImage::exportMemory(WolfVkDevice::ExternalHandle& out, std::string& error)
{
    if (!mDevice || !mAllocation.isValid() || !mAllocation.mDedicated)
    {
        error = "exportMemory of an image without its own allocation";
        return false;
    }
    return mDevice->exportMemory(mAllocation.mMemory, out, error);
}

bool WolfVkImage::createInternal(WolfVkDevice& device, Kind kind, const Format& format, uint32_t width, uint32_t height, uint32_t layers,
                                 uint32_t mip_levels, VkImageUsageFlags usage, bool dedicated, VkExternalMemoryHandleTypeFlags external, std::string& error)
{
    destroy();
    if (!format.valid() || width == 0 || height == 0 || layers == 0)
    {
        error = "WolfVkImage::create with no format or size";
        return false;
    }
    mDevice = &device;
    mKind = kind;
    mFormat = format;
    mWidth = width;
    mHeight = height;
    mLayers = layers;
    mMipLevels = std::max<uint32_t>(1, mip_levels);
    VkDevice dev = device.device();
    // [DEVICE LOST 2026-11-08 nv-hang1] VUID-VkImageCreateInfo-mipLevels-00958: mipLevels must be at most
    // floor(log2(max(width, height, depth))) + 1 (an 8x8 image was made with 6 levels in run nv-hang1). Refused here, so
    // no caller can create an image whose upper levels do not exist.
    {
        const uint32_t depth = (kind == Kind::TEXTURE_3D) ? layers : 1u;
        uint32_t largest = std::max(std::max(width, height), depth), most = 1;
        while (largest > 1) { largest >>= 1; most++; }
        if (mMipLevels > most)
        {
            error = llformat("%u mip levels asked for a %ux%u image (at most %u)", mMipLevels, width, height, most);
            mDevice = nullptr;
            return false;
        }
    }

    // The format must do what is asked of it. Source: vulkan_core.h:4534 PFN_vkGetPhysicalDeviceFormatProperties; the
    // optimalTilingFeatures bits :2715 SAMPLED_IMAGE, :2723 COLOR_ATTACHMENT_BLEND, :2724 DEPTH_STENCIL_ATTACHMENT, :2729 TRANSFER_DST
    VkFormatProperties props{};
    gVk.vkGetPhysicalDeviceFormatProperties(device.physical(), format.mFormat, &props);
    const VkFormatFeatureFlags have = props.optimalTilingFeatures;
    VkFormatFeatureFlags need = 0;
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) need |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) need |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    if (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) need |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) need |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((have & need) != need)
    {
        error = llformat("format %s lacks optimal-tiling features 0x%x (has 0x%x)", formatName(format.mFormat), need & ~have, have);
        return false;
    }

    // Source: vulkan_core.h:3898 VkImageCreateInfo { sType, pNext, flags, imageType, format, extent, mipLevels, arrayLayers, samples,
    // tiling, usage, sharingMode, queueFamilyIndexCount, pQueueFamilyIndices, initialLayout }
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;   // :221
    info.imageType = (kind == Kind::TEXTURE_3D) ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;   // :2228, :2227
    info.format = format.mFormat;
    info.extent = { width, height, (kind == Kind::TEXTURE_3D) ? layers : 1u };
    info.mipLevels = mMipLevels;
    info.arrayLayers = (kind == Kind::TEXTURE_3D) ? 1u : layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;               // :2796
    info.tiling = VK_IMAGE_TILING_OPTIMAL;              // :2219 (linear tiling cannot be shared with GL on NVIDIA; probe)
    info.usage = usage | VK_IMAGE_USAGE_SAMPLED_BIT;    // :2810
    mUsage = info.usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;     // :2270
    if (kind == Kind::TEXTURE_CUBE)
    {
        info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;   // :2766
    }
    // Source: vulkan_core.h:5951 VkExternalMemoryImageCreateInfo { sType, pNext, handleTypes }; :293 sType
    VkExternalMemoryImageCreateInfo external_info{};
    external_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external_info.handleTypes = external;
    if (external != 0)
    {
        info.pNext = &external_info;
    }
    VkResult res = gVk.vkCreateImage(dev, &info, nullptr, &mImage);
    if (res != VK_SUCCESS)
    {
        error = std::string("vkCreateImage: ") + WolfVkLoader::resultString(res);
        mImage = VK_NULL_HANDLE;
        return false;
    }

    // Memory: optimally tiled = non-linear (bufferImageGranularity, wolfvkallocator.h); dedicated when asked, or
    // when the driver prefers it (VkMemoryDedicatedRequirements, vulkan_core.h:5697, through vkGetImageMemoryRequirements2 :6175)
    VkMemoryRequirements req{};
    bool want_dedicated = dedicated;
    if (gVk.vkGetImageMemoryRequirements2)
    {
        VkMemoryDedicatedRequirements ded{};
        ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;   // :258
        VkMemoryRequirements2 req2{};
        req2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;           // :271
        req2.pNext = &ded;
        VkImageMemoryRequirementsInfo2 in{};
        in.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;  // :269
        in.image = mImage;
        gVk.vkGetImageMemoryRequirements2(dev, &in, &req2);
        req = req2.memoryRequirements;
        if (ded.prefersDedicatedAllocation || ded.requiresDedicatedAllocation) want_dedicated = true;
    }
    else
    {
        gVk.vkGetImageMemoryRequirements(dev, mImage, &req);   // :4561
    }
    if (!device.allocator().allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false /*optimal tiling*/, want_dedicated,
                                     VK_NULL_HANDLE, mImage, mAllocation, error, external))
    {
        gVk.vkDestroyImage(dev, mImage, nullptr);
        mImage = VK_NULL_HANDLE;
        return false;
    }
    if (external)
    {
        // [AMD PERF 2026-11-08] what each image shared with OpenGL got: the memory types the driver allowed for it and the
        // one taken (OpenGL draws noticeably slower into the shared images on the Radeon 890M, 8.2o)
        LL_INFOS("Vulkan") << "Shared image " << width << "x" << height << " " << formatName(format.mFormat) << " usage 0x" << std::hex << info.usage
                           << " flags 0x" << info.flags << " tiling OPTIMAL, memoryTypeBits 0x" << req.memoryTypeBits << std::dec
                           << ", memory type " << mAllocation.mMemoryType << (want_dedicated ? ", dedicated" : "") << ", " << req.size / 1024 << " KiB" << LL_ENDL;
    }
    // Source: vulkan_core.h:4559 PFN_vkBindImageMemory(device, image, memory, memoryOffset)
    res = gVk.vkBindImageMemory(dev, mImage, mAllocation.mMemory, mAllocation.mOffset);
    if (res != VK_SUCCESS)
    {
        error = std::string("vkBindImageMemory: ") + WolfVkLoader::resultString(res);
        destroy();
        return false;
    }
    // [AMD PERF 2026-11-09] debug (WolfVulkanDebugCompressionQuery): the compression the driver gave each attachment
    // image, shared or not, so the shared ones can be set beside Vulkan's own. Source: vulkan_core.h:8824
    // vkGetImageSubresourceLayout2(device, image, pSubresource, pLayout); :8503 VkImageSubresource2 { sType, pNext,
    // imageSubresource }; :8497 VkSubresourceLayout2 { sType, pNext, subresourceLayout }; :20971
    // VkImageCompressionPropertiesEXT { sType, pNext, imageCompressionFlags, imageCompressionFixedRateFlags }; sTypes :435,
    // :434, :985. One aspect bit (VUID-vkGetImageSubresourceLayout2-aspectMask-00997), DEPTH for a depth format (-04462);
    // a format with stencil would need STENCIL too (-04463), so those are not asked. Per the VkImageCompressionPropertiesEXT
    // reference page the flags are DEFAULT_EXT (0, "no fixed-rate compression"; the driver's own lossless compression may
    // or may not be on), FIXED_RATE_EXPLICIT_EXT (0x2) or DISABLED_EXT (0x4, no compression) (:20920-20923).
    if (device.compressionQuery() && !format.mStencil
        && (info.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)))
    {
        VkImageSubresource2 sub{};
        sub.sType = VK_STRUCTURE_TYPE_IMAGE_SUBRESOURCE_2;
        sub.imageSubresource.aspectMask = format.mDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        sub.imageSubresource.mipLevel = 0;
        sub.imageSubresource.arrayLayer = 0;
        VkImageCompressionPropertiesEXT comp{};
        comp.sType = VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_PROPERTIES_EXT;
        VkSubresourceLayout2 layout{};
        layout.sType = VK_STRUCTURE_TYPE_SUBRESOURCE_LAYOUT_2;
        layout.pNext = &comp;
        gVk.vkGetImageSubresourceLayout2(dev, mImage, &sub, &layout);
        const char* verdict = comp.imageCompressionFlags == VK_IMAGE_COMPRESSION_DISABLED_EXT ? "DISABLED (no compression)"
                            : comp.imageCompressionFlags == VK_IMAGE_COMPRESSION_FIXED_RATE_EXPLICIT_EXT ? "FIXED_RATE_EXPLICIT"
                            : comp.imageCompressionFlags == VK_IMAGE_COMPRESSION_DEFAULT_EXT ? "DEFAULT (no fixed-rate; lossless compression not reported)"
                            : "unexpected value";
        LL_INFOS("Vulkan") << "Compression of " << (external ? "shared" : "Vulkan-only") << " image " << width << "x" << height << " "
                           << formatName(format.mFormat) << " usage 0x" << std::hex << info.usage << ": imageCompressionFlags 0x"
                           << comp.imageCompressionFlags << " " << verdict << ", fixed-rate flags 0x" << comp.imageCompressionFixedRateFlags
                           << std::dec << "; subresource size " << layout.subresourceLayout.size / 1024 << " KiB of " << req.size / 1024
                           << " KiB allocated" << LL_ENDL;
    }

    // Source: vulkan_core.h:3939 VkImageViewCreateInfo; view types :2328 2D, :2329 3D, :2330 CUBE, :2332 2D_ARRAY
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = mImage;
    switch (kind)
    {
    case Kind::TEXTURE_CUBE: view.viewType = VK_IMAGE_VIEW_TYPE_CUBE; break;
    case Kind::TEXTURE_2D_ARRAY: view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
    case Kind::TEXTURE_3D: view.viewType = VK_IMAGE_VIEW_TYPE_3D; break;
    default: view.viewType = VK_IMAGE_VIEW_TYPE_2D; break;
    }
    view.format = format.mFormat;
    view.components = format.mSwizzle;
    // a depth/stencil image is sampled through its depth aspect only (the spec forbids both aspects in one view for sampling)
    view.subresourceRange = { format.mDepth ? static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT) : static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT),
                              0, mMipLevels, 0, info.arrayLayers };
    res = gVk.vkCreateImageView(dev, &view, nullptr, &mView);
    if (res != VK_SUCCESS)
    {
        error = std::string("vkCreateImageView: ") + WolfVkLoader::resultString(res);
        destroy();
        return false;
    }
    mLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    mLastStages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    mLastAccess = 0;
    return true;
}

void WolfVkImage::destroy()
{
    if (mDevice && mDevice->hasDevice())
    {
        WolfVkDevice* device = mDevice;
        VkImage image = mImage;
        VkImageView view = mView;
        WolfVkAllocation alloc = mAllocation;
        device->deferDestroy([device, image, view, alloc]() mutable
        {
            if (view != VK_NULL_HANDLE) gVk.vkDestroyImageView(device->device(), view, nullptr);
            if (image != VK_NULL_HANDLE) gVk.vkDestroyImage(device->device(), image, nullptr);
            device->allocator().free(alloc);
        });
    }
    mImage = VK_NULL_HANDLE;
    mView = VK_NULL_HANDLE;
    mAllocation = WolfVkAllocation();
    mDevice = nullptr;
    mLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

void WolfVkImage::transition(VkCommandBuffer cmd, VkImageLayout new_layout, VkPipelineStageFlags dst_stages, VkAccessFlags dst_access)
{
    if (mImage == VK_NULL_HANDLE) return;
    // Source: vulkan_core.h:4025 VkImageMemoryBarrier
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = mLastAccess;
    b.dstAccessMask = dst_access;
    b.oldLayout = mLayout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = mImage;
    b.subresourceRange = { aspect(), 0, mMipLevels, 0, (mKind == Kind::TEXTURE_3D) ? 1u : mLayers };
    gVk.vkCmdPipelineBarrier(cmd, mLastStages ? mLastStages : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, dst_stages, 0, 0, nullptr, 0, nullptr, 1, &b);
    mLayout = new_layout;
    mLastStages = dst_stages;
    mLastAccess = dst_access;
}

bool WolfVkImage::upload(VkCommandBuffer cmd, uint32_t mip, uint32_t layer, uint32_t width, uint32_t height, const void* data,
                         uint32_t gl_pixel_type, bool swap_bytes, std::string& error)
{
    if (mImage == VK_NULL_HANDLE || !data)
    {
        error = "upload to no image or from no data";
        return false;
    }
    // [DEVICE LOST 2026-11-08 nv-hang1] Refused, never recorded, when the copy would not fit the subresource: the mip level
    // must exist (VUID-vkCmdCopyBufferToImage-imageSubresource-07967), the layer too (-07968), and the extent must lie
    // inside that level (VUID-vkCmdCopyBufferToImage-imageSubresource-07971: a 16-wide copy into a 1x1 level in run
    // nv-hang1 was followed by VK_ERROR_DEVICE_LOST a second later). The level's size is max(1, size >> mip) (Vulkan
    // spec "Image Mip Level Sizing"); block-compressed copies must cover whole blocks or reach the level's edge, which
    // with offset 0 means a multiple of the 4x4 block or the full level (VUID-vkCmdCopyBufferToImage-dstImage-00207/-00208).
    {
        const uint32_t level_w = std::max<uint32_t>(1u, mWidth >> std::min<uint32_t>(mip, 31u));
        const uint32_t level_h = std::max<uint32_t>(1u, mHeight >> std::min<uint32_t>(mip, 31u));
        const uint32_t level_d = (mKind == Kind::TEXTURE_3D) ? std::max<uint32_t>(1u, mLayers >> std::min<uint32_t>(mip, 31u)) : mLayers;
        bool fits = mip < mMipLevels && layer < level_d && width > 0 && height > 0 && width <= level_w && height <= level_h;
        if (fits && mFormat.mBlockBytes) fits = (width == level_w || width % 4 == 0) && (height == level_h || height % 4 == 0);
        if (!fits)
        {
            error = llformat("upload refused: %ux%u into level %u layer %u of a %ux%u image with %u levels and %u layers (level is %ux%u)",
                             width, height, mip, layer, mWidth, mHeight, mMipLevels, mLayers, level_w, level_h);
            return false;
        }
    }
    const Format& f = mFormat;
    const uint32_t bpc = bytesForPixelType(gl_pixel_type);

    // Re-pack the GL rows (GL_UNPACK_ALIGNMENT 4) into a tight staging layout, widening RGB to RGBA when needed.
    VkDeviceSize staging_size = 0;
    std::vector<uint8_t> packed;
    if (f.mBlockBytes)
    {
        // BC formats: 4x4 blocks, no row padding in the GL data either (compressed data is sized by blocks)
        const uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
        staging_size = static_cast<VkDeviceSize>(bw) * bh * f.mBlockBytes;
        packed.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + staging_size);
    }
    else
    {
        if (bpc == 0 || bpc != f.mSourceBytesPerChannel)
        {
            error = llformat("pixel type 0x%x does not match format %s (%u bytes per channel expected)", gl_pixel_type, formatName(f.mFormat), f.mSourceBytesPerChannel);
            return false;
        }
        const uint32_t src_channels = f.mSourceChannels;
        const uint32_t src_texel = src_channels * bpc;
        const uint32_t src_row = (width * src_texel + 3u) & ~3u;   // GL_UNPACK_ALIGNMENT "initial value is 4" (glPixelStore reference page, registry.khronos.org/OpenGL-Refpages/gl4/html/glPixelStore.xhtml); llimagegl.cpp never sets it
        const uint32_t dst_texel = f.mTexelBytes;
        const uint32_t dst_row = width * dst_texel;
        staging_size = static_cast<VkDeviceSize>(dst_row) * height;
        packed.resize(static_cast<size_t>(staging_size));
        const uint8_t* src = static_cast<const uint8_t*>(data);
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t* s = src + static_cast<size_t>(y) * src_row;
            uint8_t* d = packed.data() + static_cast<size_t>(y) * dst_row;
            if (!f.mExpandRGB)
            {
                memcpy(d, s, dst_row);
            }
            else
            {
                // three channels in, four out; the alpha is "one" in the pixel type's own encoding
                for (uint32_t x = 0; x < width; ++x)
                {
                    memcpy(d + x * dst_texel, s + x * src_texel, src_texel);
                    uint8_t* a = d + x * dst_texel + src_texel;
                    if (bpc == 1) { a[0] = 0xFF; }
                    else if (bpc == 2) { a[0] = 0x00; a[1] = 0x3C; }           // IEEE half 1.0 = 0x3C00, little-endian
                    else { const float one = 1.0f; memcpy(a, &one, 4); }
                }
            }
            if (swap_bytes && bpc > 1)
            {
                // GL_UNPACK_SWAP_BYTES (llimagegl.cpp:811): the bytes of each component reversed
                for (uint32_t i = 0; i + bpc <= dst_row; i += bpc)
                {
                    std::reverse(d + i, d + i + bpc);
                }
            }
        }
    }

    // Staging buffer for this upload, host visible; freed frames-in-flight later by its own destroy.
    WolfVkBuffer* staging = new WolfVkBuffer();
    if (!staging->create(*mDevice, staging_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, error))
    {
        delete staging;
        return false;
    }
    staging->write(0, packed.data(), staging_size);

    const bool one_time = (cmd == VK_NULL_HANDLE);
    if (one_time)
    {
        cmd = mDevice->beginOneTime();
        if (cmd == VK_NULL_HANDLE)
        {
            error = "no one-time command buffer";
            delete staging;
            return false;
        }
    }
    if (mLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);   // :2277
    }
    // Source: vulkan_core.h:3996 VkBufferImageCopy { bufferOffset, bufferRowLength, bufferImageHeight, imageSubresource, imageOffset, imageExtent };
    // :3989 VkImageSubresourceLayers { aspectMask, mipLevel, baseArrayLayer, layerCount }. Row length 0 = tightly packed.
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource = { f.mDepth ? static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT) : static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT), mip,
                                (mKind == Kind::TEXTURE_3D) ? 0u : layer, 1 };
    region.imageOffset = { 0, 0, (mKind == Kind::TEXTURE_3D) ? static_cast<int32_t>(layer) : 0 };
    region.imageExtent = { width, height, 1 };
    // Source: vulkan_core.h:4592 PFN_vkCmdCopyBufferToImage(cmd, srcBuffer, dstImage, dstImageLayout, regionCount, pRegions)
    gVk.vkCmdCopyBufferToImage(cmd, staging->buffer(), mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    mLastStages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    mLastAccess = VK_ACCESS_TRANSFER_WRITE_BIT;

    bool ok = true;
    if (one_time)
    {
        ok = mDevice->endOneTimeAndWait(cmd, error);
    }
    // the staging buffer's destroy() defers its own release until the frames in flight are done
    staging->destroy();
    delete staging;
    return ok;
}

void WolfVkImage::generateMipmaps(VkCommandBuffer cmd)
{
    if (mImage == VK_NULL_HANDLE || mMipLevels < 2 || mFormat.mBlockBytes || mFormat.mDepth) return;
    // Blits need BLIT_SRC/BLIT_DST (vulkan_core.h:2725-2726) and linear filtering (:2727) on the format.
    VkFormatProperties props{};
    gVk.vkGetPhysicalDeviceFormatProperties(mDevice->physical(), mFormat.mFormat, &props);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((props.optimalTilingFeatures & need) != need)
    {
        LL_WARNS_ONCE("Vulkan") << "generateMipmaps: format " << formatName(mFormat.mFormat) << " cannot be blitted linearly" << LL_ENDL;
        return;
    }
    const uint32_t layers = (mKind == Kind::TEXTURE_3D) ? 1u : mLayers;
    // level 0 must be transfer-src, the rest transfer-dst; the whole image is in TRANSFER_DST after upload
    if (mLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    }
    int32_t w = static_cast<int32_t>(mWidth), h = static_cast<int32_t>(mHeight);
    for (uint32_t level = 1; level < mMipLevels; ++level)
    {
        VkImageMemoryBarrier to_src{};
        to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_src.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;   // :3115
        to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;   // :2276
        to_src.srcQueueFamilyIndex = to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_src.image = mImage;
        to_src.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 1, 0, layers };
        gVk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_src);

        const int32_t nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        // Source: vulkan_core.h:4505 VkImageBlit { srcSubresource, srcOffsets[2], dstSubresource, dstOffsets[2] }
        VkImageBlit blit{};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, layers };
        blit.srcOffsets[0] = { 0, 0, 0 };
        blit.srcOffsets[1] = { w, h, 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, layers };
        blit.dstOffsets[0] = { 0, 0, 0 };
        blit.dstOffsets[1] = { nw, nh, 1 };
        // Source: vulkan_core.h:4660 PFN_vkCmdBlitImage(cmd, srcImage, srcLayout, dstImage, dstLayout, regionCount, pRegions, filter)
        gVk.vkCmdBlitImage(cmd, mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);   // :2374
        w = nw; h = nh;
    }
    // every level but the last is TRANSFER_SRC now; bring them all to TRANSFER_DST so the image has one layout again
    VkImageMemoryBarrier back{};
    back.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    back.srcQueueFamilyIndex = back.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    back.image = mImage;
    back.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mMipLevels - 1, 0, layers };
    gVk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &back);
    mLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    mLastStages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    mLastAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
}

bool WolfVkImage::readBack(std::vector<uint8_t>& out, std::string& error)
{
    if (mImage == VK_NULL_HANDLE)
    {
        error = "readBack of no image";
        return false;
    }
    const uint32_t texel = mFormat.mBlockBytes ? 0 : (mFormat.mDepth ? 4u : mFormat.mTexelBytes);
    if (texel == 0)
    {
        error = "readBack of a block-compressed image is not supported";
        return false;
    }
    if (!(mUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
    {
        // a copy out needs TRANSFER_SRC (VUID-vkCmdCopyImageToBuffer-srcImage-00186) and the TRANSFER_SRC layout needs it too
        // (VUID-VkImageMemoryBarrier-oldLayout-01212): both seen 2026-11-08 from the debug probe on a mirrored texture
        error = "readBack of an image created without VK_IMAGE_USAGE_TRANSFER_SRC_BIT";
        return false;
    }
    const VkDeviceSize size = static_cast<VkDeviceSize>(mWidth) * mHeight * texel;
    WolfVkBuffer host;
    if (!host.create(*mDevice, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, error))
    {
        return false;
    }
    VkCommandBuffer cmd = mDevice->beginOneTime();
    if (cmd == VK_NULL_HANDLE)
    {
        error = "no one-time command buffer";
        return false;
    }
    const VkImageLayout previous = mLayout;
    const VkPipelineStageFlags prev_stages = mLastStages;
    const VkAccessFlags prev_access = mLastAccess;
    transition(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = { mFormat.mDepth ? static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT) : static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT), 0, 0, 1 };
    region.imageExtent = { mWidth, mHeight, 1 };
    // Source: vulkan_core.h:4593 PFN_vkCmdCopyImageToBuffer(cmd, srcImage, srcImageLayout, dstBuffer, regionCount, pRegions)
    gVk.vkCmdCopyImageToBuffer(cmd, mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, host.buffer(), 1, &region);
    // host read of the buffer after the copy: :2936 HOST_BIT, :3117 HOST_READ
    VkBufferMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = host.buffer();
    b.size = VK_WHOLE_SIZE;
    gVk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &b, 0, nullptr);
    // back to where it was, if it was somewhere useful
    if (previous != VK_IMAGE_LAYOUT_UNDEFINED && previous != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
    {
        transition(cmd, previous, prev_stages ? prev_stages : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, prev_access);
    }
    if (!mDevice->endOneTimeAndWait(cmd, error))
    {
        return false;
    }
    out.resize(static_cast<size_t>(size));
    memcpy(out.data(), host.hostData(), static_cast<size_t>(size));
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// WolfVkSamplerCache

void WolfVkSamplerCache::destroy()
{
    if (mDevice && mDevice->hasDevice())
    {
        for (auto& kv : mSamplers)
        {
            gVk.vkDestroySampler(mDevice->device(), kv.second, nullptr);
        }
    }
    mSamplers.clear();
    mDevice = nullptr;
}

VkSampler WolfVkSamplerCache::get(Filter filter, Address address, bool has_mips, bool shadow_compare)
{
    const uint32_t key = (filter & 3u) | ((address & 3u) << 2) | (has_mips ? 16u : 0u) | (shadow_compare ? 32u : 0u);
    auto it = mSamplers.find(key);
    if (it != mSamplers.end())
    {
        return it->second;
    }
    // Source: vulkan_core.h:4140 VkSamplerCreateInfo { sType, pNext, flags, magFilter, minFilter, mipmapMode, addressModeU, addressModeV,
    // addressModeW, mipLodBias, anisotropyEnable, maxAnisotropy, compareEnable, compareOp, minLod, maxLod, borderColor, unnormalizedCoordinates }
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;   // :238
    // llrender.cpp:506-540 setTextureFilteringOptionFast: POINT -> NEAREST mag + NEAREST(_MIPMAP_NEAREST) min; the others LINEAR mag;
    // TRILINEAR/ANISOTROPIC min LINEAR_MIPMAP_LINEAR; BILINEAR min LINEAR_MIPMAP_NEAREST with mips, LINEAR without
    const bool point = filter == TFO_POINT;
    info.magFilter = point ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;   // :2373-2374
    info.minFilter = info.magFilter;
    info.mipmapMode = (filter >= TFO_TRILINEAR) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;   // :2405, :2404
    VkSamplerAddressMode mode = VK_SAMPLER_ADDRESS_MODE_REPEAT;   // :2381, sGLAddressMode GL_REPEAT
    if (address == TAM_MIRROR) mode = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;   // :2382 GL_MIRRORED_REPEAT
    if (address == TAM_CLAMP) mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;      // :2383 GL_CLAMP_TO_EDGE
    info.addressModeU = info.addressModeV = info.addressModeW = mode;
    info.mipLodBias = 0.f;
    // llrender.cpp:543-551: GL_TEXTURE_MAX_ANISOTROPY = gGLManager.mMaxAnisotropy for TFO_ANISOTROPIC, 1.0 otherwise
    info.anisotropyEnable = (filter == TFO_ANISOTROPIC && mMaxAnisotropy > 1.f) ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy = info.anisotropyEnable ? mMaxAnisotropy : 1.f;
    info.compareEnable = shadow_compare ? VK_TRUE : VK_FALSE;
    info.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;   // :2395 = pipeline.cpp:1150-1151 GL_TEXTURE_COMPARE_MODE GL_COMPARE_R_TO_TEXTURE, GL_TEXTURE_COMPARE_FUNC GL_LEQUAL (shadow maps)
    info.minLod = 0.f;
    // without mips the GL min filter has no mip part: clamp to level 0, so a texture with levels still samples like GL
    info.maxLod = has_mips ? VK_LOD_CLAMP_NONE : 0.f;   // :125
    info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;   // :2365 (clamp-to-edge never reads it)
    info.unnormalizedCoordinates = VK_FALSE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkResult res = gVk.vkCreateSampler(mDevice->device(), &info, nullptr, &sampler);
    if (res != VK_SUCCESS)
    {
        LL_WARNS("Vulkan") << "vkCreateSampler: " << WolfVkLoader::resultString(res) << LL_ENDL;
        return VK_NULL_HANDLE;
    }
    mSamplers[key] = sampler;
    return sampler;
}
