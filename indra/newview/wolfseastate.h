/**
 * @file wolfseastate.h
 * @brief WolfViewer: the ONE mapping from a region's EEP water settings to the wave field.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef WOLF_SEASTATE_H
#define WOLF_SEASTATE_H

#include "llsettingswater.h"
#include "v2math.h"
#include "v3math.h"
#include <cmath>

// Source: wolfstorm/js/world/sea_state.js SeaState — the same numbers, kept in step.
//
// EEP carries no wave height. It does carry wave1_direction (direction AND scroll speed
// of the first ripple layer, default (1.04999, -0.42) — llsettingswater.cpp:107) and
// normal_scale (ripple strength, default 2 — llsettingswater.cpp:104). A region whose
// author made the water scroll fast with strong ripples means a rougher sea, one with
// slow faint ripples a calm one; the swell, its wavelength, its chaos and the spectral
// cascades' wind are derived from those relative to the Firestorm defaults, which map to
// EXACTLY the wave parameters this viewer shipped with (0.22 m at a 20 m dominant
// wavelength — settings.xml WolfViewerWaterWaveHeight / WolfViewerWaterWaveScale). A
// region that never touched its water looks as it did.
//
// Custom keys are not an option: OpenSim's WaterData (OpenSim/Framework/ViewerWater.cs
// FromWLOSD) copies only the known fields, so anything else is dropped before a viewer
// sees it. DECLARED CONVENTION, not Firestorm behaviour (Firestorm has no wave geometry).
struct WolfSeaState
{
    F32 mIndex = 1.f;
    F32 mAmplitude = 0.22f;    // metres, swell
    F32 mFrequency = 0.1f;     // 1 / base wavelength (m); the dominant train is 2x
    F32 mSpeed = 1.f;
    F32 mChaos = 0.f;
    F32 mDirX = 0.9284755f;    // unit swell direction (EEP wave1_direction normalised)
    F32 mDirY = -0.3713937f;
    F32 mWindSpeed = 6.f;      // JONSWAP wind at 10 m, m/s

    // Source: sea_state.js SeaState constants.
    static constexpr F32 DEFAULT_WAVE1_LEN = 1.1308968f;   // hypot(1.04999, -0.42)
    static constexpr F32 DEFAULT_NORMAL_SCALE = 2.0f;
    static constexpr F32 BASE_AMPLITUDE = 0.22f;
    static constexpr F32 BASE_FREQUENCY = 0.1f;
    static constexpr F32 MIN_INDEX = 0.15f;
    static constexpr F32 MAX_INDEX = 3.0f;

    // Source: sea_state.js SeaState.fromWater()
    static WolfSeaState fromWater(const LLSettingsWater::ptr_t& water)
    {
        WolfSeaState st;
        LLVector2 w1(1.04999f, -0.42000f);
        LLVector3 ns(2.f, 2.f, 2.f);
        if (water)
        {
            w1 = water->getWave1Dir();
            ns = water->getNormalScale();
        }
        const F32 w1len = w1.length();
        // Scroll speed relative to the default: the author's "how fast does it move".
        const F32 speed_idx = (w1len > 1e-4f ? w1len : DEFAULT_WAVE1_LEN) / DEFAULT_WAVE1_LEN;
        // Ripple strength relative to the default, square rooted: the default day track
        // already wobbles normal_scale by +-12% and a sea that breathed with it would look
        // like weather changing hourly.
        const F32 ns_mean = (ns.mV[VX] + ns.mV[VY]) * 0.5f;
        const F32 rough_idx = sqrtf(llmax(ns_mean, 0.05f) / DEFAULT_NORMAL_SCALE);
        st.mIndex = llclamp(speed_idx * rough_idx, MIN_INDEX, MAX_INDEX);
        return fromIndex(st.mIndex, w1);
    }

    // Everything but the direction follows the index; the menu-free path for a manual
    // wave height (settings) goes through here too.
    static WolfSeaState fromIndex(F32 index, LLVector2 dir)
    {
        WolfSeaState st;
        st.mIndex = llclamp(index, MIN_INDEX, MAX_INDEX);
        st.mAmplitude = BASE_AMPLITUDE * powf(st.mIndex, 1.5f);
        st.mFrequency = BASE_FREQUENCY / sqrtf(st.mIndex);
        st.mSpeed = 1.f;
        st.mChaos = llclamp((st.mIndex - 1.f) * 0.5f, 0.f, 0.8f);
        // 6 m/s at the default state, 3.45 dead calm, 12 in the roughest (measured with
        // wolfstorm/tools/water_preview.html, 2026-09-06).
        st.mWindSpeed = 3.f + 3.f * st.mIndex;
        if (dir.length() < 1e-4f)
        {
            dir.set(1.04999f, -0.42000f);
        }
        dir.normalize();
        st.mDirX = dir.mV[VX];
        st.mDirY = dir.mV[VY];
        return st;
    }

    // The index a manual wave height corresponds to (inverse of fromIndex's amplitude).
    static F32 indexForAmplitude(F32 amplitude_m)
    {
        return llclamp(powf(llmax(amplitude_m, 0.001f) / BASE_AMPLITUDE, 2.f / 3.f), MIN_INDEX, MAX_INDEX);
    }
};

#endif // WOLF_SEASTATE_H
