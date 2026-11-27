/**
 * @file wolfcoreloader.h
 * @brief Dynamic runtime loader for the optional WolfCore acceleration library.
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

#ifndef WOLF_CORELOADER_H
#define WOLF_CORELOADER_H

#include "wolfcore_api.h"
#include <string>

namespace WolfCoreLoader
{
    /**
     * Lazily attempts once to load the optional WolfCore shared library from the viewer's own
     * installation folder:
     *   - Windows: <exe_dir>\wolfcore.dll
     *   - macOS:   <bundle>/Contents/Frameworks/libwolfcore.dylib
     *   - Linux:   <exe_dir>/../lib/libwolfcore.so (or <exe_dir>/libwolfcore.so)
     *
     * Returns nullptr when WolfCore is not installed (e.g. open-source builds from GitHub), in
     * which case callers use their LGPL-2.1 open-source fallback implementations.
     */
    const WolfCoreApi* api();

    /** Path of the loaded WolfCore library, or empty when not present. */
    const std::string& loadedPath();
}

#endif // WOLF_CORELOADER_H
