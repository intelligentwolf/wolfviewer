# -*- cmake -*-
include(Linking)
include(Prebuilt)
include(CheckCXXSourceCompiles)

include_guard()
add_library( ll::cef INTERFACE IMPORTED )

use_prebuilt_binary(dullahan)
# Always prefer ${AUTOBUILD_INSTALL_DIR}/include/cef so dullahan.h ABI matches
# ${AUTOBUILD_INSTALL_DIR}/lib/release/libdullahan.a even if LIBS_PREBUILT_DIR was cached from another build tree.
if (EXISTS "${AUTOBUILD_INSTALL_DIR}/include/cef/dullahan.h")
    set(_WOLF_CEF_INCLUDE_DIR "${AUTOBUILD_INSTALL_DIR}/include/cef")
else ()
    set(_WOLF_CEF_INCLUDE_DIR "${LIBS_PREBUILT_DIR}/include/cef")
endif ()
target_include_directories( ll::cef SYSTEM INTERFACE "${_WOLF_CEF_INCLUDE_DIR}")

# Source: indra/media_plugins/cef/media_plugin_cef.cpp — audio_to_host and the four
# callbacks are optional Dullahan APIs. Test the installed header rather than assuming
# a package version carries them, so the normal CEF audio path remains buildable.
set(_WOLF_CEF_REQUIRED_INCLUDES "${CMAKE_REQUIRED_INCLUDES}")
set(_WOLF_CEF_TRY_COMPILE_TYPE "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
list(APPEND CMAKE_REQUIRED_INCLUDES "${_WOLF_CEF_INCLUDE_DIR}")
unset(WOLF_DULLAHAN_AUDIO_TO_HOST CACHE)
check_cxx_source_compiles(
    "#include <dullahan.h>
     #include <cstdint>
     #include <functional>
     #include <string>
     int main()
     {
         dullahan::dullahan_settings settings;
         settings.audio_to_host = true;
         dullahan browser;
         browser.setOnAudioStreamStartedCallback(std::function<void(int, int, int)>());
         browser.setOnAudioStreamPacketCallback(std::function<void(const float**, int, int, int64_t)>());
         browser.setOnAudioStreamStoppedCallback(std::function<void()>());
         browser.setOnAudioStreamErrorCallback(std::function<void(const std::string)>());
         return 0;
     }"
    WOLF_DULLAHAN_AUDIO_TO_HOST)
set(CMAKE_REQUIRED_INCLUDES "${_WOLF_CEF_REQUIRED_INCLUDES}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_WOLF_CEF_TRY_COMPILE_TYPE}")
unset(_WOLF_CEF_REQUIRED_INCLUDES)
unset(_WOLF_CEF_TRY_COMPILE_TYPE)

if (WOLF_DULLAHAN_AUDIO_TO_HOST)
    target_compile_definitions(ll::cef INTERFACE WOLF_DULLAHAN_AUDIO_TO_HOST=1)
endif ()

if (WINDOWS)
    target_link_libraries( ll::cef INTERFACE
        libcef.lib
        libcef_dll_wrapper.lib
        dullahan.lib
    )
elseif (DARWIN)
    FIND_LIBRARY(APPKIT_LIBRARY AppKit)
    if (NOT APPKIT_LIBRARY)
        message(FATAL_ERROR "AppKit not found")
    endif()

    set(CEF_LIBRARY "'${ARCH_PREBUILT_DIRS_RELEASE}/Chromium\ Embedded\ Framework.framework'")
    if (NOT CEF_LIBRARY)
        message(FATAL_ERROR "CEF not found")
    endif()

    target_link_libraries( ll::cef INTERFACE
        ${ARCH_PREBUILT_DIRS_RELEASE}/libcef_dll_wrapper.a
        ${ARCH_PREBUILT_DIRS_RELEASE}/libdullahan.a
        ${APPKIT_LIBRARY}
       )

elseif (LINUX)
    target_link_libraries( ll::cef INTERFACE
        dullahan
        cef
        cef_dll_wrapper.a
    )
endif (WINDOWS)
