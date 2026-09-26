# -*- cmake -*-
include(Prebuilt)
include(Linking)

include_guard()
add_library( ll::libcurl INTERFACE IMPORTED )

use_system_binary(libcurl)
use_prebuilt_binary(curl)

find_library(CURL_LIBRARY
    NAMES
    libcurl.lib
    libcurl.a
    PATHS "${ARCH_PREBUILT_DIRS_RELEASE}" REQUIRED NO_DEFAULT_PATH)

target_link_libraries(ll::libcurl INTERFACE ${CURL_LIBRARY} ll::openssl ll::nghttp2 ll::zlib-ng)
# <WolfViewer 2026-09-26> curl 8.22 (static) calls if_nametoindex (peer.c), which lives in
# Iphlpapi.lib; curl's own static link list names iphlpapi on Windows.
if (WINDOWS)
  target_link_libraries(ll::libcurl INTERFACE iphlpapi)
endif (WINDOWS)
# </WolfViewer>

target_include_directories( ll::libcurl SYSTEM INTERFACE ${LIBS_PREBUILT_DIR}/include)
