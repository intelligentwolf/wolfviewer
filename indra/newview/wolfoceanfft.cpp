/**
 * @file wolfoceanfft.cpp
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

#include "llviewerprecompiledheaders.h"

#include "wolfoceanfft.h"

#include "llglslshader.h"
#include "llimagegl.h"
#include "llrender.h"
#include "llshadermgr.h"
#include "llviewershadermgr.h"
#include "pipeline.h"

#include <cmath>

// MSVC ships no pi constant without _USE_MATH_DEFINES; use the viewer's own (llmath.h F_PI, F_SQRT2).
static const double WOLF_PI_D = 3.14159265358979323846;

// Source: wolfstorm/js/world/ocean_fft.js OceanFFT static constants.
const F32 WolfOceanFFT::TILES[WolfOceanFFT::CASCADES] = { 96.f, 12.f };
const F32 WolfOceanFFT::BANDS[WolfOceanFFT::CASCADES][2] = { { 4.f, 96.f }, { 0.25f, 4.f } };
const F32 WolfOceanFFT::FADE[WolfOceanFFT::CASCADES] = { 400.f, 60.f };

WolfOceanFFT::WolfOceanFFT()
{
}

WolfOceanFFT::~WolfOceanFFT()
{
    release();
}

void WolfOceanFFT::release()
{
    for (S32 c = 0; c < CASCADES; ++c)
    {
        Cascade& cas = mCascades[c];
        if (cas.mH0Tex)
        {
            LLImageGL::deleteTextures(1, &cas.mH0Tex);
            cas.mH0Tex = 0;
        }
        for (S32 i = 0; i < 2; ++i)
        {
            cas.mSpec[i].release();
            cas.mFin[i].release();
        }
        cas.mBuilt = false;
        cas.mRan = false;
        cas.mCur = 0;
    }
    mAllocated = false;
}

bool WolfOceanFFT::isReady() const
{
    return mAllocated && !mFailed && mCascades[0].mRan && mCascades[1].mRan;
}

LLRenderTarget* WolfOceanFFT::finalTarget(S32 c)
{
    if (c < 0 || c >= CASCADES || !mCascades[c].mRan)
    {
        return nullptr;
    }
    return &mCascades[c].mFin[mCascades[c].mCur];
}

// Source: ocean_fft.js OceanFFT._rng — mulberry32, so both viewers draw the same sea.
U32 WolfOceanFFT::rngNext(U32& a)
{
    a += 0x6D2B79F5u;
    U32 t = a;
    t = (t ^ (t >> 15)) * (t | 1u);
    t ^= t + (t ^ (t >> 7)) * (t | 61u);
    return t ^ (t >> 14);
}

// Source: ocean_fft.js OceanFFT._lgamma — Lanczos (g = 7, n = 9).
F32 WolfOceanFFT::lgammaf_(F32 xf)
{
    static const double c[9] = { 0.99999999999980993, 676.5203681218851, -1259.1392167224028,
        771.32342877765313, -176.61502916214059, 12.507343278686905,
        -0.13857109526572012, 9.9843695780195716e-6, 1.5056327351493116e-7 };
    double x = xf;
    if (x < 0.5)
    {
        return (F32)(log(WOLF_PI_D / sin(WOLF_PI_D * x)) - lgammaf_((F32)(1.0 - x)));
    }
    x -= 1.0;
    double a = c[0];
    const double t = x + 7.5;
    for (int i = 1; i < 9; ++i) a += c[i] / (x + i);
    return (F32)(0.5 * log(2.0 * WOLF_PI_D) + (x + 0.5) * log(t) - t + log(a));
}

// Source: ocean_fft.js OceanFFT._jonswap — Hasselmann et al. 1973 as in Horvath 2015 eq. 28-31.
F32 WolfOceanFFT::jonswap(F32 w, F32 U, F32 F, F32& wp_out)
{
    const F32 g = G;
    const F32 wp = 22.f * powf(g * g / (U * F), 1.f / 3.f);
    const F32 alpha = 0.076f * powf(U * U / (F * g), 0.22f);
    const F32 sigma = w <= wp ? 0.07f : 0.09f;
    const F32 r = expf(-((w - wp) * (w - wp)) / (2.f * sigma * sigma * wp * wp));
    const F32 S = alpha * g * g / powf(w, 5.f) * expf(-1.25f * powf(wp / w, 4.f)) * powf(GAMMA, r);
    wp_out = wp;
    return S;
}

// Source: ocean_fft.js OceanFFT._spread — Hasselmann 1980 spreading with Horvath's swell term.
F32 WolfOceanFFT::spread(F32 theta, F32 w, F32 wp, F32 U, F32 theta_w)
{
    const F32 g = G;
    const F32 ratio = w / wp;
    F32 s;
    if (w <= wp)
    {
        s = 6.97f * powf(ratio, 4.06f);
    }
    else
    {
        const F32 mu = -2.33f - 1.45f * (U * wp / g - 1.17f);
        s = 9.77f * powf(ratio, mu);
    }
    s += 16.f * tanhf(wp / w) * SWELL * SWELL;
    s = llclamp(s, 0.1f, 200.f);
    const F32 lnQ = (2.f * s - 1.f) * F_LN2 - logf(F_PI)
                  + 2.f * lgammaf_(s + 1.f) - lgammaf_(2.f * s + 1.f);
    const F32 half = 0.5f * (theta - theta_w);
    const F32 c = fabsf(cosf(half));
    return expf(lnQ) * powf(c, 2.f * s);
}

// Source: ocean_fft.js OceanFFT._buildH0 — |h0(k)| = sqrt(2 S D (dw/dk) / k) dk with the
// MEASURED halving (see that function: the rendered RMS came out at 2x the band's sigma
// without it), a seeded complex Gaussian, band-limited with a half-texel soft edge, and
// conj(h0(-k)) stored beside h0(k).
void WolfOceanFFT::buildH0(S32 c)
{
    Cascade& cas = mCascades[c];
    const S32 n = N;
    const F32 L = TILES[c];
    const F32 lmin = BANDS[c][0], lmax = BANDS[c][1];
    const F32 kmin = 2.f * F_PI / lmax, kmax = 2.f * F_PI / lmin;
    const F32 dk = 2.f * F_PI / L;
    const F32 theta_w = atan2f(mState.mDirY, mState.mDirX);
    const F32 U = llmax(mState.mWindSpeed, 1.f), F = FETCH_M;
    const F32 g = G;

    mScratchGauss.assign((size_t)n * n * 2, 0.f);
    U32 seed = SEED + (U32)c * 7919u;
    for (S32 i = 0; i < n * n; ++i)
    {
        const F32 u1 = llmax((F32)rngNext(seed) / 4294967296.f, 1e-7f);
        const F32 u2 = (F32)rngNext(seed) / 4294967296.f;
        const F32 r = sqrtf(-2.f * logf(u1));
        mScratchGauss[i * 2] = r * cosf(2.f * F_PI * u2);
        mScratchGauss[i * 2 + 1] = r * sinf(2.f * F_PI * u2);
    }
    mScratchH0.assign((size_t)n * n * 2, 0.f);
    for (S32 j = 0; j < n; ++j)
    {
        const F32 ky = (j - n / 2) * dk;
        for (S32 i = 0; i < n; ++i)
        {
            const F32 kx = (i - n / 2) * dk;
            const F32 k = sqrtf(kx * kx + ky * ky);
            const S32 o = (j * n + i) * 2;
            if (k < 1e-6f) continue;
            const F32 band = llclamp((k - kmin) / dk + 0.5f, 0.f, 1.f)
                           * llclamp((kmax - k) / dk + 0.5f, 0.f, 1.f);
            if (band <= 0.f) continue;
            const F32 w = sqrtf(g * k);
            F32 wp;
            const F32 S = jonswap(w, U, F, wp);
            const F32 D = spread(atan2f(ky, kx), w, wp, U, theta_w);
            const F32 dwdk = g / (2.f * w);
            const F32 amp = sqrtf(llmax(2.f * S * D * dwdk / k, 0.f)) * dk * band;
            const F32 s = amp / (2.f * F_SQRT2);
            mScratchH0[o] = mScratchGauss[o] * s;
            mScratchH0[o + 1] = mScratchGauss[o + 1] * s;
        }
    }
    mScratchData.assign((size_t)n * n * 4, 0.f);
    for (S32 j = 0; j < n; ++j)
    {
        const S32 jm = (n - j) % n;
        for (S32 i = 0; i < n; ++i)
        {
            const S32 im = (n - i) % n;
            const S32 o = j * n + i, om = jm * n + im;
            mScratchData[o * 4] = mScratchH0[o * 2];
            mScratchData[o * 4 + 1] = mScratchH0[o * 2 + 1];
            mScratchData[o * 4 + 2] = mScratchH0[om * 2];
            mScratchData[o * 4 + 3] = -mScratchH0[om * 2 + 1];
        }
    }
    if (!cas.mH0Tex)
    {
        LLImageGL::generateTextures(1, &cas.mH0Tex);
    }
    gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, cas.mH0Tex);
    LLImageGL::setManualImage(LLTexUnit::getInternalType(LLTexUnit::TT_TEXTURE), 0, GL_RGBA32F,
                              n, n, GL_RGBA, GL_FLOAT, mScratchData.data(), false);
    gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_POINT);
    gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    cas.mBuilt = true;
}

// Source: ocean_fft.js OceanFFT.setSeaState — rebuild on a change of a quarter of a metre
// per second or ~2 degrees; chop and the whitecap threshold follow the sea-state index
// (measured curve: 0% of the tile below 0.60 at index 1, 3.3% below 0.68 at index 3).
void WolfOceanFFT::setSeaState(const WolfSeaState& st)
{
    if (mHaveState
        && fabsf(mState.mWindSpeed - st.mWindSpeed) < 0.25f
        && fabsf(mState.mDirX - st.mDirX) < 0.035f
        && fabsf(mState.mDirY - st.mDirY) < 0.035f)
    {
        return;
    }
    mState = st;
    mHaveState = true;
    const F32 x = llclamp(st.mIndex - 1.f, 0.f, 2.f);
    mChoppiness = 1.0f + 0.3f * x;
    mFoamThreshold = 0.60f + 0.04f * x;
    for (S32 c = 0; c < CASCADES; ++c)
    {
        mCascades[c].mBuilt = false;
    }
}

bool WolfOceanFFT::allocate()
{
    if (mAllocated || mFailed)
    {
        return mAllocated;
    }
    // Source: llrendertarget.cpp allocate()/addColorAttachment() — a second attachment on
    // the same target is the MRT the butterflies need; attachment 0 defaults to bilinear
    // and the rest to point, so the finished textures set their own filtering below.
    for (S32 c = 0; c < CASCADES; ++c)
    {
        Cascade& cas = mCascades[c];
        for (S32 i = 0; i < 2; ++i)
        {
            if (!cas.mSpec[i].allocate(N, N, GL_RGBA32F, false) || !cas.mSpec[i].addColorAttachment(GL_RGBA32F)
                || !cas.mFin[i].allocate(N, N, GL_RGBA16F, false) || !cas.mFin[i].addColorAttachment(GL_RGBA16F))
            {
                LL_WARNS("WolfOceanFFT") << "float render targets unavailable — spectral ocean disabled" << LL_ENDL;
                mFailed = true;
                release();
                return false;
            }
            for (U32 a = 0; a < 2; ++a)
            {
                gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, cas.mFin[i].getTexture(a), true);
                gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_TRILINEAR);
                gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_WRAP);
                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
                // Mip storage for the trilinear filter: allocate the chain once, empty.
                gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, cas.mFin[i].getTexture(a), true);
                glGenerateMipmap(GL_TEXTURE_2D);
                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            }
            // Clear the finished targets so the first foam read is zero.
            cas.mFin[i].bindTarget();
            glClearColor(0.f, 0.f, 0.f, 0.f);
            cas.mFin[i].clear(GL_COLOR_BUFFER_BIT);
            cas.mFin[i].flush();
        }
    }
    mAllocated = true;
    LL_INFOS("WolfOceanFFT") << "initialised: " << CASCADES << " cascades x " << N << "x" << N
                             << ", tiles " << TILES[0] << "/" << TILES[1] << " m" << LL_ENDL;
    return true;
}

// Source: ocean_fft.js OceanFFT.update — one cascade per call (alternating): time pass,
// 7 horizontal + 7 vertical Stockham stages ping-ponging the two spec targets, then the
// finalise pass into the other finished target, reading the last one's foam.
void WolfOceanFFT::update(F32 time)
{
    if (!mHaveState || !allocate())
    {
        return;
    }
    if (!gWolfFFTTimeProgram.isComplete() || !gWolfFFTButterflyProgram.isComplete() || !gWolfFFTFinalProgram.isComplete())
    {
        return;
    }
    LL_PROFILE_GPU_ZONE("wolf ocean fft");
    for (S32 c = 0; c < CASCADES; ++c)
    {
        if (!mCascades[c].mBuilt)
        {
            buildH0(c);
        }
    }
    LLGLDepthTest depth(GL_FALSE, GL_FALSE);
    LLGLDisable blend(GL_BLEND);
    LLGLDisable cull(GL_CULL_FACE);
    gGL.setColorMask(true, true);

    ++mFrame;
    const S32 c = (S32)(mFrame & 1u);
    Cascade& cas = mCascades[c];
    const F32 elapsed = cas.mRan ? llclamp(time - cas.mLastTime, 0.f, 0.2f) : (1.f / 60.f);
    cas.mLastTime = time;

    static LLStaticHashedString s_uTime("uTime");
    static LLStaticHashedString s_uN("uN");
    static LLStaticHashedString s_uL("uL");
    static LLStaticHashedString s_uNs("uNs");
    static LLStaticHashedString s_uNi("uN");
    static LLStaticHashedString s_uHorizontal("uHorizontal");
    static LLStaticHashedString s_uLambda("uLambda");
    static LLStaticHashedString s_uFoamDecay("uFoamDecay");
    static LLStaticHashedString s_uFoamGain("uFoamGain");
    static LLStaticHashedString s_uFoamThreshold("uFoamThreshold");
    static LLStaticHashedString s_uDt("uDt");

    // 1. time evolution -> spec[0]
    {
        LLGLSLShader& sh = gWolfFFTTimeProgram;
        cas.mSpec[0].bindTarget();
        sh.bind();
        S32 chan = sh.enableTexture(LLShaderMgr::WOLF_FFT_H0);
        if (chan > -1)
        {
            gGL.getTexUnit(chan)->bindManual(LLTexUnit::TT_TEXTURE, cas.mH0Tex);
        }
        sh.uniform1f(s_uTime, time);
        sh.uniform1f(s_uN, (F32)N);
        sh.uniform1f(s_uL, TILES[c]);
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        sh.disableTexture(LLShaderMgr::WOLF_FFT_H0);
        sh.unbind();
        cas.mSpec[0].flush();
    }
    // 2. butterflies
    S32 src = 0;
    {
        LLGLSLShader& sh = gWolfFFTButterflyProgram;
        for (S32 pass = 0; pass < 2; ++pass)
        {
            for (S32 Ns = 1; Ns < N; Ns <<= 1)
            {
                cas.mSpec[1 - src].bindTarget();
                sh.bind();
                sh.bindTexture(LLShaderMgr::WOLF_FFT_IN0, &cas.mSpec[src], false, LLTexUnit::TFO_POINT, 0);
                sh.bindTexture(LLShaderMgr::WOLF_FFT_IN1, &cas.mSpec[src], false, LLTexUnit::TFO_POINT, 1);
                sh.uniform1i(s_uNs, Ns);
                sh.uniform1i(s_uNi, N);
                sh.uniform1i(s_uHorizontal, pass == 0 ? 1 : 0);
                gPipeline.mScreenTriangleVB->setBuffer();
                gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
                sh.unbind();
                cas.mSpec[1 - src].flush();
                src = 1 - src;
            }
        }
    }
    // 3. finalise -> fin[1 - cur], reading fin[cur]'s foam
    {
        LLGLSLShader& sh = gWolfFFTFinalProgram;
        LLRenderTarget& dst = cas.mFin[1 - cas.mCur];
        dst.bindTarget();
        sh.bind();
        sh.bindTexture(LLShaderMgr::WOLF_FFT_IN0, &cas.mSpec[src], false, LLTexUnit::TFO_POINT, 0);
        sh.bindTexture(LLShaderMgr::WOLF_FFT_IN1, &cas.mSpec[src], false, LLTexUnit::TFO_POINT, 1);
        sh.bindTexture(LLShaderMgr::WOLF_FFT_PREV, &cas.mFin[cas.mCur], false, LLTexUnit::TFO_POINT, 0);
        sh.uniform1f(s_uLambda, mChoppiness);
        sh.uniform1f(s_uFoamDecay, powf(0.5f, elapsed / FOAM_HALF_LIFE));
        sh.uniform1f(s_uFoamGain, FOAM_GAIN);
        sh.uniform1f(s_uFoamThreshold, mFoamThreshold);
        sh.uniform1f(s_uDt, elapsed);
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        sh.unbind();
        dst.flush();
        // Mipmaps for both attachments: far water samples an averaged, near-flat surface
        // instead of sparkling (LLRenderTarget::flush only regenerates attachment 0, and
        // only with TMG_AUTO).
        for (U32 a = 0; a < 2; ++a)
        {
            gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, dst.getTexture(a), true);
            glGenerateMipmap(GL_TEXTURE_2D);
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        }
        cas.mCur = 1 - cas.mCur;
        cas.mRan = true;
    }
}
