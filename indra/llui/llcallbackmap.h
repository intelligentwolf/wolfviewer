/**
 * @file llcallbackmap.h
 * @brief LLCallbackMap base class
 *
 * $LicenseInfo:firstyear=2006&license=viewerlgpl$
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

#ifndef LLCALLBACKMAP_H
#define LLCALLBACKMAP_H

#include "llstl.h"

#include <string>
#include <functional>
#include <map>
#include <unordered_map>

class LLCallbackMap
{
public:
    // callback definition.
    typedef std::function<void* (void* data)> callback_t;

    // [W113 2026-11-25] std::map, as before Firestorm 7.2.5 (0e687a83b5b made it std::unordered_map): the value type is
    // this class itself, still incomplete here, and g++-11's libstdc++ (our Linux CI, ubuntu-22.04) cannot instantiate an
    // unordered_map's node for an incomplete type (CI run 45638976878, linux64-avx2: stl_pair.h "second has incomplete type").
    // std::map accepts it, as it did up to W111.
    typedef std::map<std::string, LLCallbackMap> map_t;
    typedef map_t::iterator map_iter_t;
    typedef map_t::const_iterator map_const_iter_t;

    template <class T>
    static void* buildPanel(void* data)
    {
        T* panel = new T();
        return (void*)panel;
    }

    LLCallbackMap() : mCallback(nullptr), mData(nullptr) {}
    LLCallbackMap(callback_t callback, void* data = nullptr) : mCallback(callback), mData(data) {}

    callback_t  mCallback;
    void*       mData;
};

#endif // LLCALLBACKMAP_H
