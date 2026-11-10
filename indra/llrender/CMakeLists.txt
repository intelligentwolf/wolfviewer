# -*- cmake -*-

project(llrender)

include(00-Common)
include(OpenGL)
include(FreeType)
include(LLCommon)
include(LLImage)
include(LLWindow)
include(Vulkan)   # [VULKAN BACKEND 2026-11-08/] vendored headers for vk/

set(llrender_SOURCE_FILES
    llcubemap.cpp
    llcubemaparray.cpp
    llfontbitmapcache.cpp
    llfontfreetype.cpp
    llfontfreetypesvg.cpp
    llfontgl.cpp
    llfontvertexbuffer.cpp
    llfontregistry.cpp
    llgl.cpp
    llglslshader.cpp
    llgltexture.cpp
    llimagegl.cpp
    llpostprocess.cpp
    llrender.cpp
    llrender2dutils.cpp
    llrendernavprim.cpp
    llrendersphere.cpp
    llrendertarget.cpp
    llshadermgr.cpp
    lltexture.cpp
    lltexturemanagerbridge.cpp
    lluiimage.cpp
    llvertexbuffer.cpp
    llglcommonfunc.cpp
    vk/wolfvkloader.cpp
    vk/wolfvkallocator.cpp
    vk/wolfvkdevice.cpp
    vk/wolfvksurface.cpp
    vk/wolfvkswapchain.cpp
    vk/wolfvkspirv.cpp
    vk/wolfvkbuffer.cpp
    vk/wolfvkimage.cpp
    vk/wolfvkrendertarget.cpp
    vk/wolfvkpipeline.cpp
    vk/wolfvkshader.cpp
    )

# [VULKAN BACKEND 2026-11-08] the Metal-layer helper for VK_EXT_metal_surface is Objective-C++
if (DARWIN)
  list(APPEND llrender_SOURCE_FILES vk/wolfvksurface_macosx.mm)
endif (DARWIN)
    
set(llrender_HEADER_FILES
    CMakeLists.txt

    llcubemap.h
    llcubemaparray.h
    llfontgl.h
    llfontvertexbuffer.h
    llfontfreetype.h
    llfontfreetypesvg.h
    llfontbitmapcache.h
    llfontregistry.h
    llgl.h
    llglheaders.h
    llglslshader.h
    llglstates.h
    llgltexture.h
    llgltypes.h
    llimagegl.h
    llpostprocess.h
    llrender.h
    llrender2dutils.h
    llrendernavprim.h
    llrendersphere.h
    llshadermgr.h
    lltexture.h
    lltexturemanagerbridge.h
    lluiimage.h
    lluiimage.inl
    llvertexbuffer.h
    llglcommonfunc.h
    vk/wolfvkloader.h
    vk/wolfvkallocator.h
    vk/wolfvkdevice.h
    vk/wolfvksurface.h
    vk/wolfvkswapchain.h
    vk/wolfvkspirv.h
    vk/wolfvkbuffer.h
    vk/wolfvkimage.h
    vk/wolfvkrendertarget.h
    vk/wolfvkpipeline.h
    vk/wolfvkshader.h
    )

list(APPEND llrender_SOURCE_FILES ${llrender_HEADER_FILES})

if (BUILD_HEADLESS)
  add_library (llrenderheadless
    ${llrender_SOURCE_FILES}
    )

  set_property(TARGET llrenderheadless
    PROPERTY COMPILE_DEFINITIONS LL_MESA=1 LL_MESA_HEADLESS=1
      )

  target_link_libraries(llrenderheadless
          llcommon
          llimage
          llmath
          llrender
          llxml
          llfilesystem
          )
endif (BUILD_HEADLESS)

add_library (llrender ${llrender_SOURCE_FILES})
target_include_directories( llrender  INTERFACE   ${CMAKE_CURRENT_SOURCE_DIR})

set_property(TARGET llrender PROPERTY POSITION_INDEPENDENT_CODE ON)

# Libraries on which this library depends, needed for Linux builds
# Sort by high-level to low-level
target_link_libraries(llrender
        llcommon
        llimage
        llmath
        llfilesystem
        llxml
        llwindow
        ll::freetype
        ll::vulkan_headers
        OpenGL::GL
        OpenGL::GLU
        )

