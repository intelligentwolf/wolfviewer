/**
 * @file wolfsurfcurl.cpp
 * @brief WolfViewer: the barrel — curl ribbons along the surf train's break line.
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
#include "wolfsurfcurl.h"

#include "llagent.h"
#include "llglslshader.h"
#include "llglstates.h"
#include "llrender.h"
#include "llshadermgr.h"
#include "llviewercamera.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llframetimer.h"
#include "wolfwaterfield.h"
#include "wolfwavezones.h"

#include <cmath>
#include <map>
#include <string>

WolfSurfCurl::WolfSurfCurl() {}
WolfSurfCurl::~WolfSurfCurl() { release(); }

void WolfSurfCurl::release()
{
    mVB = nullptr;
    mIndexCount = 0;
    mVertexCount = 0;
    mBuiltAt = -1.0;
    mBuiltHandle = 0;
    mRibbons = 0;
    mPoints = 0;
}

// Source: surf_curl.js breakDepth — waterV.glsl [SURF]: breakF = 1 once crestH / (0.78 (h + 0.8 H))
// >= 1.15 with crestH capped at 1.15 H, i.e. h <= 0.48 H; the dissipation ramp
// smoothstep(0.2, 0.6 + 0.5 H, h) still leaves 0.9 of the crest at 0.6 H.
F32 WolfSurfCurl::breakDepth(F32 surf_h)
{
    return llclamp(0.6f * surf_h, 0.5f, 40.f);
}

namespace
{
    // Source: surf_curl.js _rebuild — the same marching squares and chaining.
    struct Seg { F32 x0, y0, x1, y1; };

    inline std::string key_of(F32 x, F32 y)
    {
        return std::to_string((S32)std::lround(x * 2.f)) + "," + std::to_string((S32)std::lround(y * 2.f));
    }
    inline F32 lerp_zero(F32 a, F32 b, F32 fa, F32 fb)
    {
        return a + (b - a) * (fa / (fa - fb));
    }
}

bool WolfSurfCurl::rebuild(F32 surf_h)
{
    LLViewerRegion* rgn = gAgent.getRegion();
    const WolfWaterField::Field* fld = rgn ? WolfWaterField::instance().get(rgn) : nullptr;
    const S32 RES = WolfWaterField::RES, ERES = WolfWaterField::ERES;
    mRibbons = 0;
    mPoints = 0;
    mIndexCount = 0;
    mVertexCount = 0;
    if (!fld || !fld->mReady || fld->mDepth.size() != (size_t)RES * RES * 4
        || fld->mExpo.size() != (size_t)ERES * ERES * 2)
    {
        mVB = nullptr;
        return false;
    }
    const std::vector<F32>& data = fld->mDepth;
    const std::vector<F32>& expo = fld->mExpo;
    const F32 sx = fld->mSizeX, sy = fld->mSizeY, wl = fld->mWaterLevel;
    const F32 hB = breakDepth(surf_h);

    // Field: f = depth - hB on the RES^2 grid (A channel = smoothed height).
    std::vector<F32> f((size_t)RES * RES);
    for (S32 j = 0; j < RES; ++j)
        for (S32 i = 0; i < RES; ++i)
            f[(size_t)j * RES + i] = (wl - data[((size_t)j * RES + i) * 4 + 3]) - hB;
    const F32 cellX = sx / (RES - 1), cellY = sy / (RES - 1);

    // Marching squares: segments in region metres.
    std::vector<Seg> segs;
    for (S32 j = 0; j + 1 < RES; ++j)
    {
        for (S32 i = 0; i + 1 < RES; ++i)
        {
            const F32 f00 = f[(size_t)j * RES + i], f10 = f[(size_t)j * RES + i + 1];
            const F32 f01 = f[(size_t)(j + 1) * RES + i], f11 = f[(size_t)(j + 1) * RES + i + 1];
            const S32 c = (f00 > 0 ? 1 : 0) | (f10 > 0 ? 2 : 0) | (f11 > 0 ? 4 : 0) | (f01 > 0 ? 8 : 0);
            if (c == 0 || c == 15) continue;
            const F32 x0 = i * cellX, y0 = j * cellY;
            F32 px[4], py[4];
            S32 n = 0;
            if ((c & 1) != ((c >> 1) & 1)) { px[n] = lerp_zero(x0, x0 + cellX, f00, f10); py[n] = y0; ++n; }
            if (((c >> 1) & 1) != ((c >> 2) & 1)) { px[n] = x0 + cellX; py[n] = lerp_zero(y0, y0 + cellY, f10, f11); ++n; }
            if (((c >> 3) & 1) != ((c >> 2) & 1)) { px[n] = lerp_zero(x0, x0 + cellX, f01, f11); py[n] = y0 + cellY; ++n; }
            if ((c & 1) != ((c >> 3) & 1)) { px[n] = x0; py[n] = lerp_zero(y0, y0 + cellY, f00, f01); ++n; }
            if (n >= 2) segs.push_back({ px[0], py[0], px[1], py[1] });
            if (n == 4) segs.push_back({ px[2], py[2], px[3], py[3] });
        }
    }

    // Chain into polylines (endpoint hashing on a 0.5 m grid).
    std::map<std::string, std::vector<S32>> ends;
    for (S32 idx = 0; idx < (S32)segs.size(); ++idx)
    {
        ends[key_of(segs[idx].x0, segs[idx].y0)].push_back(idx);
        ends[key_of(segs[idx].x1, segs[idx].y1)].push_back(idx);
    }
    std::vector<U8> used(segs.size(), 0);
    std::vector<std::vector<std::pair<F32, F32>>> polylines;
    auto extend = [&](std::vector<std::pair<F32, F32>>& line, F32 x, F32 y)
    {
        for (;;)
        {
            auto it = ends.find(key_of(x, y));
            S32 next = -1;
            if (it != ends.end())
                for (S32 idx : it->second) if (!used[idx]) { next = idx; break; }
            if (next < 0) return;
            used[next] = 1;
            const Seg& s = segs[next];
            const bool at_start = key_of(s.x0, s.y0) == key_of(x, y);
            x = at_start ? s.x1 : s.x0;
            y = at_start ? s.y1 : s.y0;
            line.push_back({ x, y });
        }
    };
    for (S32 i = 0; i < (S32)segs.size(); ++i)
    {
        if (used[i]) continue;
        used[i] = 1;
        const Seg& s = segs[i];
        std::vector<std::pair<F32, F32>> line = { { s.x0, s.y0 }, { s.x1, s.y1 } };
        extend(line, s.x1, s.y1);
        std::vector<std::pair<F32, F32>> rev = { { s.x1, s.y1 }, { s.x0, s.y0 } };
        extend(rev, s.x0, s.y0);
        std::reverse(rev.begin(), rev.end());
        rev.pop_back(); rev.pop_back();
        rev.insert(rev.end(), line.begin(), line.end());
        polylines.push_back(std::move(rev));
    }

    // Samplers. Direction of travel = up the smoothed-height gradient (toward rising terrain).
    auto grad_at = [&](F32 x, F32 y, F32& gx, F32& gy) -> bool
    {
        const S32 ti = llclamp((S32)std::lround(x / cellX), 1, RES - 2);
        const S32 tj = llclamp((S32)std::lround(y / cellY), 1, RES - 2);
        auto a = [&](S32 ii, S32 jj) { return data[((size_t)jj * RES + ii) * 4 + 3]; };
        gx = (a(ti + 1, tj) - a(ti - 1, tj)) / (2.f * cellX);
        gy = (a(ti, tj + 1) - a(ti, tj - 1)) / (2.f * cellY);
        const F32 l = std::sqrt(gx * gx + gy * gy);
        if (l < 1e-4f) return false;
        gx /= l; gy /= l;
        return true;
    };
    // Distance to land, bilinear (a nearest read quantises the phase along the line).
    auto dist_at = [&](F32 x, F32 y, F32& out) -> bool
    {
        const F32 uu = (x - fld->mExpoX0) / fld->mExpoSX, vv = (y - fld->mExpoY0) / fld->mExpoSY;
        if (uu < 0.f || uu > 1.f || vv < 0.f || vv > 1.f) return false;
        const F32 fx = uu * (ERES - 1), fy = vv * (ERES - 1);
        const S32 i0 = llmin(ERES - 2, (S32)fx), j0 = llmin(ERES - 2, (S32)fy);
        const F32 tx = fx - i0, ty = fy - j0;
        auto d = [&](S32 ii, S32 jj) { return expo[((size_t)jj * ERES + ii) * 2 + 1]; };
        out = (d(i0, j0) * (1 - tx) + d(i0 + 1, j0) * tx) * (1 - ty) + (d(i0, j0 + 1) * (1 - tx) + d(i0 + 1, j0 + 1) * tx) * ty;
        return true;
    };
    auto in_surf = [&](F32 x, F32 y) { return WolfWaterField::zoneAt(*fld, x, y) >= 0.9f; };   // painted surf cells only

    mPos.clear(); mUV.clear(); mDir.clear(); mDist.clear(); mIdx.clear();
    S32 along = 0, ribbons = 0;
    for (const auto& line : polylines)
    {
        // arc-length resample
        std::vector<std::pair<F32, F32>> pts;
        F32 carry = 0.f;
        for (size_t i = 0; i + 1 < line.size(); ++i)
        {
            const F32 ax = line[i].first, ay = line[i].second, bx = line[i + 1].first, by = line[i + 1].second;
            const F32 len = std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
            if (len < 1e-6f) continue;
            F32 d = carry;
            while (d <= len)
            {
                const F32 t = d / len;
                pts.push_back({ ax + (bx - ax) * t, ay + (by - ay) * t });
                d += RIBBON_STEP_M;
            }
            carry = d - len;
        }
        // runs inside surf cells with a valid direction and distance
        std::vector<Pt> run;
        auto flush = [&]()
        {
            if ((F32)run.size() * RIBBON_STEP_M >= MIN_RIBBON_M && along + (S32)run.size() <= MAX_ALONG_POINTS)
            {
                appendRibbon(run, 0.f);
                along += (S32)run.size();
                ++ribbons;
            }
            run.clear();
        };
        for (const auto& p : pts)
        {
            F32 gx, gy, dd;
            if (grad_at(p.first, p.second, gx, gy) && dist_at(p.first, p.second, dd) && in_surf(p.first, p.second))
                run.push_back({ p.first, p.second, gx, gy, dd });
            else
                flush();
        }
        flush();
    }
    mRibbons = ribbons;
    mPoints = along;
    mVertexCount = (U32)(mPos.size() / 3);
    mIndexCount = (U32)mIdx.size();
    if (mVertexCount == 0 || mIndexCount == 0 || mVertexCount > 65535)
    {
        mVB = nullptr;
        return false;
    }

    // Source: pipeline.cpp LLVertexBuffer recipe (wolfwakefield.cpp mQuadVB).
    mVB = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_TEXCOORD0
                             | LLVertexBuffer::MAP_TEXCOORD1 | LLVertexBuffer::MAP_TEXCOORD2);
    if (!mVB->allocateBuffer(mVertexCount, mIndexCount))
    {
        LL_WARNS("WolfSurfCurl") << "vertex buffer allocation failed (" << mVertexCount << " verts)" << LL_ENDL;
        mVB = nullptr;
        return false;
    }
    LLStrider<LLVector3> vert;
    LLStrider<LLVector2> uv, dir, dist;
    LLStrider<U16> idx;
    mVB->getVertexStrider(vert);
    mVB->getTexCoord0Strider(uv);
    mVB->getTexCoord1Strider(dir);
    mVB->getTexCoord2Strider(dist);
    mVB->getIndexStrider(idx);
    for (U32 v = 0; v < mVertexCount; ++v)
    {
        vert[v].set(mPos[v * 3], mPos[v * 3 + 1], mPos[v * 3 + 2]);
        uv[v].set(mUV[v * 2], mUV[v * 2 + 1]);
        dir[v].set(mDir[v * 2], mDir[v * 2 + 1]);
        dist[v].set(mDist[v], 0.f);
    }
    for (U32 k = 0; k < mIndexCount; ++k) idx[k] = mIdx[k];
    mVB->unmapBuffer();
    LL_INFOS("WolfSurfCurl") << mRibbons << " ribbons, " << mPoints << " points along the " << hB << " m break line" << LL_ENDL;
    return true;
}

// Source: surf_curl.js _appendRibbon — N points along x PROFILE_STEPS across; attributes:
// position (x, y, 0), uv (s along metres, u across 0..1), dir, dist.
void WolfSurfCurl::appendRibbon(const std::vector<Pt>& run, F32 s0)
{
    const S32 M = PROFILE_STEPS;
    const U16 vbase = (U16)(mPos.size() / 3);
    F32 s = s0;
    for (size_t i = 0; i < run.size(); ++i)
    {
        const Pt& p = run[i];
        if (i > 0) s += std::sqrt((p.x - run[i - 1].x) * (p.x - run[i - 1].x) + (p.y - run[i - 1].y) * (p.y - run[i - 1].y));
        for (S32 m = 0; m < M; ++m)
        {
            mPos.push_back(p.x); mPos.push_back(p.y); mPos.push_back(0.f);
            mDir.push_back(p.dx); mDir.push_back(p.dy);
            mDist.push_back(p.dist);
            mUV.push_back(s); mUV.push_back((F32)m / (F32)(M - 1));
        }
    }
    for (size_t i = 0; i + 1 < run.size(); ++i)
    {
        for (S32 m = 0; m + 1 < M; ++m)
        {
            const U16 a = (U16)(vbase + i * M + m), b = (U16)(a + M);
            mIdx.push_back(a); mIdx.push_back(b); mIdx.push_back((U16)(a + 1));
            mIdx.push_back((U16)(a + 1)); mIdx.push_back(b); mIdx.push_back((U16)(b + 1));
        }
    }
}

void WolfSurfCurl::render(F32 surf_h, F32 surf_set, F32 surf_len, F32 phase_time,
                          const LLVector3& light_dir, const LLColor3& light_diffuse,
                          const LLSettingsWater::ptr_t& pwater, LLViewerTexture* normal_map)
{
    if (!gWolfSurfCurlProgram.isComplete()) return;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return;
    const WolfWaterField::Field* fld = WolfWaterField::instance().get(rgn);
    if (!fld || !fld->mReady || !fld->mZoneTex) return;

    // Rebuild when the fields rebaked, the region changed or the height changed (a slider).
    // (at most twice a second: a slider drag changes the height every frame)
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if ((fld->mBakedAt != mBuiltAt || rgn->getHandle() != mBuiltHandle || std::fabs(surf_h - mBuiltH) > 0.005f)
        && now >= mNextRebuild)
    {
        mNextRebuild = now + 0.5;
        mBuiltAt = fld->mBakedAt;
        mBuiltHandle = rgn->getHandle();
        mBuiltH = surf_h;
        rebuild(surf_h);
    }
    if (!mVB || mIndexCount == 0) return;

    static LLStaticHashedString s_time("time");
    static LLStaticHashedString s_surf_height("surfHeight");
    static LLStaticHashedString s_surf_set("surfSetInterval");
    static LLStaticHashedString s_surf_len("surfLength");
    static LLStaticHashedString s_surf_speed("surfSpeed");
    static LLStaticHashedString s_water_level("waterLevel");
    static LLStaticHashedString s_h_break("hBreak");
    static LLStaticHashedString s_region_origin("wolfRegionOrigin");
    static LLStaticHashedString s_water_color("curlWaterColor");
    static LLStaticHashedString s_sun_color("curlSunColor");
    static LLStaticHashedString s_eye("curlEye");

    LLGLSLShader& sh = gWolfSurfCurlProgram;
    sh.bind();
    const LLVector3 origin = rgn->getOriginAgent();
    sh.uniform1f(s_time, phase_time);
    sh.uniform1f(s_surf_height, surf_h);
    sh.uniform1f(s_surf_set, surf_set);
    sh.uniform1f(s_surf_len, surf_len);
    sh.uniform1f(s_surf_speed, 1.f);
    sh.uniform1f(s_water_level, fld->mWaterLevel);
    sh.uniform1f(s_h_break, breakDepth(surf_h));
    sh.uniform2f(s_region_origin, origin.mV[VX], origin.mV[VY]);
    // The sea's fog colour and the sun, in linear light like waterF.glsl's own terms.
    const LLColor3 fog_linear = linearColor3(pwater->getWaterFogColor());
    const LLColor3 sun_linear = linearColor3(light_diffuse);
    sh.uniform3fv(s_water_color, 1, fog_linear.mV);
    sh.uniform3fv(s_sun_color, 1, sun_linear.mV);
    sh.uniform3fv(LLShaderMgr::WATER_LIGHT_DIR, 1, light_dir.mV);
    sh.uniform3fv(s_eye, 1, LLViewerCamera::getInstance()->getOrigin().mV);
    if (normal_map)
    {
        sh.bindTexture(LLShaderMgr::BUMP_MAP, normal_map);
    }

    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    LLGLDepthTest depth(GL_TRUE, GL_TRUE);
    mVB->setBuffer();
    mVB->drawRange(LLRender::TRIANGLES, 0, mVertexCount - 1, mIndexCount, 0);
    sh.unbind();
}
