# -*- cmake -*-
# [VULKAN BACKEND 2026-11-08] Vulkan headers for the native Vulkan render backend (indra/llrender/vk).
#
# Headers only: the Khronos Vulkan-Headers 1.4.363 are vendored under indra/llrender/vk/third_party/vulkan-headers
# (Apache-2.0 OR MIT; LICENSE.txt beside them), so every platform build (Windows, macOS, Linux) sees the same files
# and the "vulkan_core.h:LINE" citations in the backend stay true everywhere. No Vulkan SDK is needed to build.
#
# No link-time dependency: the backend opens the Vulkan loader at run time (wolfvkloader.cpp: dlopen libvulkan.so.1,
# LoadLibrary vulkan-1.dll, dlopen libvulkan.1.dylib/libMoltenVK.dylib) and fetches every entry point through
# vkGetInstanceProcAddr, so a PC without Vulkan still runs the OpenGL viewer. VK_NO_PROTOTYPES keeps link-time
# prototypes out of every translation unit that includes the headers.
#
# Pattern: like OpenXR.cmake / VulkanGltf.cmake this defines one imported interface target, ll::vulkan_headers, but
# from the in-tree copy instead of an autobuild package, because a header set needs no per-platform binary.
# [CI 2026-11-09] include/vk_video/ too (all twelve vulkan_video_codec*.h of the same tag, v1.4.363, byte-identical to
# github.com/KhronosGroup/Vulkan-Headers at that tag): vulkan_core.h includes "vk_video/vulkan_video_codec_h264std.h"
# (vulkan_core.h:9805 and on), found through the include/ directory below; the macOS CI build failed without them
# (run 42595767793), Linux had only been finding them in /usr/include.
# To update the headers: copy vk_platform.h, vulkan.h, vulkan_core.h, vulkan_beta.h, vulkan_xlib.h, vulkan_wayland.h,
# vulkan_win32.h, vulkan_metal.h, include/vk_video/*.h and the licence from one Vulkan-Headers release, then re-run
# scripts/vulkan/wolfvk_cite.py to refresh the line citations (it fails the build check if a cited line moved).

include_guard()
add_library( ll::vulkan_headers INTERFACE IMPORTED )

set(WOLF_VULKAN_HEADERS_DIR ${CMAKE_SOURCE_DIR}/llrender/vk/third_party/vulkan-headers/include)
target_include_directories( ll::vulkan_headers SYSTEM INTERFACE ${WOLF_VULKAN_HEADERS_DIR} )
target_compile_definitions( ll::vulkan_headers INTERFACE VK_NO_PROTOTYPES=1 )

if (DARWIN)
  # wolfvksurface_macosx.mm attaches a CAMetalLayer to the GL view for VK_EXT_metal_surface (MoltenVK).
  find_library(QUARTZCORE_LIBRARY QuartzCore)
  target_link_libraries( ll::vulkan_headers INTERFACE ${QUARTZCORE_LIBRARY} )
endif (DARWIN)
