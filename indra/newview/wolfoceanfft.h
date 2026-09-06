/**
 * @file wolfoceanfft.h
 * @brief WolfViewer: spectral (JONSWAP / inverse FFT) wind-sea cascades for the region water.
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

#ifndef WOLF_OCEANFFT_H
#define WOLF_OCEANFFT_H

#include "llsingleton.h"
#include "llrendertarget.h"
#include "wolfseastate.h"

// Source: wolfstorm/js/world/ocean_fft.js OceanFFT — the same pipeline, the same numbers,
// kept in step. See that file's header for the technique (Tessendorf 2001; Horvath 2015;
// GodotOceanWaves; Tidewater) and why: a sum of eight Gerstner trains is a fine authored
// swell but a corrugated wind sea; a spectrum gives the wind sea thousands of components
// for the price of a 128x128 inverse FFT, done here as fragment passes on LLRenderTargets
// (no compute shaders needed — GL 3.x is enough).
//
// Per update, one cascade (alternating): a time-evolution pass, 14 Stockham butterfly
// passes (7 per axis, both targets together via MRT) and a finalise pass that writes
//   attachment 0 = (dx, dy, dz, accumulated foam)     attachment 1 = (dz/dx, dz/dy, J, 0)
// into world-tiling, mipmapped RGBA16F textures the water shader samples by world XY / tile.
class WolfOceanFFT : public LLSingleton<WolfOceanFFT>
{
    LLSINGLETON(WolfOceanFFT);
    ~WolfOceanFFT();

public:
    static constexpr S32 N = 128;
    static constexpr S32 CASCADES = 2;
    static const F32 TILES[CASCADES];          // metres per tile
    static const F32 BANDS[CASCADES][2];       // wavelengths carried, metres
    static const F32 FADE[CASCADES];           // displacement fade-out distance, metres
    static constexpr F32 G = 9.81f;
    static constexpr F32 FETCH_M = 100000.f;
    static constexpr F32 GAMMA = 3.3f;
    static constexpr F32 SWELL = 0.35f;
    static constexpr F32 FOAM_GAIN = 3.0f;
    static constexpr F32 FOAM_HALF_LIFE = 3.2f;
    static constexpr U32 SEED = 1337;

    /** The wind the spectra blow with (wolfseastate.h). Rebuilds h0 on a real change. */
    void setSeaState(const WolfSeaState& st);
    /** Advance one cascade. `time` is the water clock (LLDrawPoolWater WATER_TIME x wave
     *  speed). Called once per frame from the water pool, before the water shader binds. */
    void update(F32 time);
    /** True once both cascades have run at least once. */
    bool isReady() const;
    /** The finished target for cascade c (attachment 0 = displacement, 1 = derivatives). */
    LLRenderTarget* finalTarget(S32 c);
    F32 getTile(S32 c) const { return TILES[c]; }
    F32 getFade(S32 c) const { return FADE[c]; }
    /** Free GL resources (shutdown, shader reload); the next update reallocates. */
    void release();

private:
    struct Cascade
    {
        U32 mH0Tex = 0;
        LLRenderTarget mSpec[2];
        LLRenderTarget mFin[2];
        S32 mCur = 0;
        bool mBuilt = false;
        bool mRan = false;
        F32 mLastTime = 0.f;
    };

    bool allocate();
    void buildH0(S32 c);
    static F32 lgammaf_(F32 x);
    static F32 jonswap(F32 w, F32 U, F32 F, F32& wp_out);
    static F32 spread(F32 theta, F32 w, F32 wp, F32 U, F32 theta_w);
    static U32 rngNext(U32& a);

    Cascade mCascades[CASCADES];
    bool mAllocated = false;
    bool mFailed = false;
    bool mHaveState = false;
    WolfSeaState mState;
    F32 mChoppiness = 1.0f;
    F32 mFoamThreshold = 0.60f;
    U32 mFrame = 0;
    std::vector<F32> mScratchH0, mScratchGauss, mScratchData;
};

#endif // WOLF_OCEANFFT_H
