/**
 * @file wolfwakefield.cpp
 * @brief WolfViewer: world-locked boat wash / wake field for the region water.
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

#include "wolfwakefield.h"

#include "llagent.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llshadermgr.h"
#include "llviewercamera.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "pipeline.h"

#include <algorithm>
#include <cmath>

WolfWakeField::WolfWakeField()
{
}

WolfWakeField::~WolfWakeField()
{
    release();
}

void WolfWakeField::release()
{
    mRT[0].release();
    mRT[1].release();
    mQuadVB = nullptr;
    mAllocated = false;
    mReady = false;
    mBoats.clear();
}

bool WolfWakeField::allocate()
{
    if (mAllocated || mFailed)
    {
        return mAllocated;
    }
    // Source: wake_field.js WakeField._init — RGBA8 targets, not float: 8 bits per channel
    // is ample for a foam mask and the diffuse pass hides the quantisation.
    for (S32 i = 0; i < 2; ++i)
    {
        if (!mRT[i].allocate(RES, RES, GL_RGBA8, false))
        {
            LL_WARNS("WolfWakeField") << "render target unavailable — boat wash disabled" << LL_ENDL;
            mFailed = true;
            release();
            return false;
        }
        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mRT[i].getTexture(0));
        gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
        gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    }
    // Source: wake_field.js — one reusable unit quad (-0.5..0.5); the stamp shader shapes
    // the wake inside it. Source: pipeline.cpp:559-568 for the LLVertexBuffer recipe.
    mQuadVB = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX);
    if (!mQuadVB->allocateBuffer(4, 6))
    {
        mFailed = true;
        release();
        return false;
    }
    LLStrider<LLVector3> vert;
    LLStrider<U16> idx;
    mQuadVB->getVertexStrider(vert);
    mQuadVB->getIndexStrider(idx);
    vert[0].set(-0.5f, -0.5f, 0.f);
    vert[1].set(0.5f, -0.5f, 0.f);
    vert[2].set(0.5f, 0.5f, 0.f);
    vert[3].set(-0.5f, 0.5f, 0.f);
    idx[0] = 0; idx[1] = 1; idx[2] = 2;
    idx[3] = 0; idx[4] = 2; idx[5] = 3;
    mQuadVB->unmapBuffer();
    mAllocated = true;
    clear();
    LL_INFOS("WolfWakeField") << "initialised " << RES << "x" << RES << " RGBA8 ping-pong" << LL_ENDL;
    return true;
}

void WolfWakeField::clear()
{
    if (!mAllocated)
    {
        return;
    }
    // Source: wake_field.js _clearTargets — neutral water is all-zero.
    for (S32 i = 0; i < 2; ++i)
    {
        mRT[i].bindTarget();
        glClearColor(0.f, 0.f, 0.f, 0.f);
        mRT[i].clear(GL_COLOR_BUFFER_BIT);
        mRT[i].flush();
    }
}

// Source: wake_field.js WakeField._isBoat — a MOVING PHYSICAL ROOT PRIM whose hull straddles
// the water surface; no name matching, no guessing (SL/OpenSim has no vehicle object type).
bool WolfWakeField::isBoat(LLViewerObject* obj, F32 water_z)
{
    if (!obj || obj->isDead() || obj->isAvatar())
    {
        return false;
    }
    if (obj->isAttachment() || obj->getParent())
    {
        return false;
    }
    const LLVector3& v = obj->getVelocity();
    const F32 speed = sqrtf(v.mV[VX] * v.mV[VX] + v.mV[VY] * v.mV[VY]);
    if (speed < MIN_SPEED)
    {
        return false;
    }
    const F32 half_h = llmax(0.5f, obj->getScale().mV[VZ] * 0.5f);
    return fabsf(obj->getPositionRegion().mV[VZ] - water_z) <= half_h + 1.5f;
}

// Source: wake_field.js WakeField.update
void WolfWakeField::update(F32 dt)
{
    if (!(dt > 0.f) || mFailed)
    {
        return;
    }
    LLViewerRegion* regionp = gAgent.getRegion();
    if (!regionp)
    {
        return;
    }
    if (!gWolfWakeDecayProgram.isComplete() || !gWolfWakeStampProgram.isComplete())
    {
        return;
    }
    dt = llmin(dt, 0.1f);
    if (!allocate())
    {
        return;
    }
    if (regionp->getHandle() != mRegionHandle)
    {
        mRegionHandle = regionp->getHandle();
        mBoats.clear();
        clear();
    }
    mRegionSizeX = regionp->getWidth();
    mRegionSizeY = regionp->getWidth();
    const F32 water_z = regionp->getWaterHeight();
    // <WolfViewer 2026-10-06> The field's window: the whole region up to FIELD_M, else FIELD_M
    // about the camera, moved in whole texels once the camera is an eighth of it from the middle.
    {
        const F32 size = llmin(mRegionSizeX, FIELD_M);
        if (size != mFieldSize)
        {
            mFieldSize = size;
            mFieldOX = mFieldOY = 0.f;
            mShiftU = mShiftV = 0.f;
            clear();
        }
        if (size < mRegionSizeX)
        {
            const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin() - regionp->getOriginAgent();
            const F32 texel = size / RES;
            auto place = [&](F32 c, F32 o) -> F32
            {
                if (fabsf(c - (o + size * 0.5f)) <= size * 0.125f) return o;
                return llclamp(floorf((c - size * 0.5f) / texel + 0.5f) * texel, 0.f, mRegionSizeX - size);
            };
            const F32 ox = place(cam.mV[VX], mFieldOX), oy = place(cam.mV[VY], mFieldOY);
            mShiftU += (ox - mFieldOX) / size;
            mShiftV += (oy - mFieldOY) / size;
            mFieldOX = ox;
            mFieldOY = oy;
        }
    }

    std::vector<LLViewerObject*> boats;
    for (const LLPointer<LLViewerObject>& objp : gObjectList.getActiveObjects())
    {
        LLViewerObject* obj = objp.get();
        if (obj && obj->getRegion() == regionp && isBoat(obj, water_z))
        {
            boats.push_back(obj);
        }
    }
    // <WolfViewer 2026-09-26> Forget boats that have gone (derezzed, left, stopped floating):
    // mBoats used to keep one entry per boat ever seen until the next region change.
    for (auto it = mBoats.begin(); it != mBoats.end();)
    {
        const U32 local_id = it->first;
        const bool live = std::any_of(boats.begin(), boats.end(),
                                      [local_id](LLViewerObject* b) { return b->getLocalID() == local_id; });
        it = live ? std::next(it) : mBoats.erase(it);
    }
    // Early-out once the field has provably faded (several half-lives with no boat).
    mIdleSecs = boats.empty() ? mIdleSecs + dt : 0.f;
    const F32 settle_secs = llmax(FOAM_HALF_LIFE, CREST_HALF_LIFE) * 4.f;
    if (boats.empty() && mIdleSecs > settle_secs)
    {
        mShiftU = mShiftV = 0.f;   // the field is clean water: nothing to carry along
        return;
    }
    if ((S32)boats.size() > MAX_BOATS)
    {
        const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin() - regionp->getOriginAgent();
        std::sort(boats.begin(), boats.end(), [&](LLViewerObject* a, LLViewerObject* b) {
            const LLVector3 pa = a->getPositionRegion() - cam, pb = b->getPositionRegion() - cam;
            return pa.mV[VX] * pa.mV[VX] + pa.mV[VY] * pa.mV[VY] < pb.mV[VX] * pb.mV[VX] + pb.mV[VY] * pb.mV[VY];
        });
        boats.resize(MAX_BOATS);
    }

    LL_PROFILE_GPU_ZONE("wolf wake field");
    LLGLDepthTest depth(GL_FALSE, GL_FALSE);
    LLGLDisable cull(GL_CULL_FACE);
    gGL.setColorMask(true, true);

    static LLStaticHashedString s_uFoamDecay("uFoamDecay");
    static LLStaticHashedString s_uCrestDecay("uCrestDecay");
    static LLStaticHashedString s_uTexel("uTexel");
    static LLStaticHashedString s_uSpread("uSpread");
    static LLStaticHashedString s_uShift("uShift");

    // PASS 1: decay + diffuse into the other target.
    {
        LLGLDisable blend(GL_BLEND);
        LLGLSLShader& sh = gWolfWakeDecayProgram;
        LLRenderTarget& dst = mRT[1 - mCur];
        dst.bindTarget();
        sh.bind();
        sh.bindTexture(LLShaderMgr::WOLF_FFT_PREV, &mRT[mCur], false, LLTexUnit::TFO_BILINEAR, 0);
        sh.uniform1f(s_uFoamDecay, powf(0.5f, dt / FOAM_HALF_LIFE));
        sh.uniform1f(s_uCrestDecay, powf(0.5f, dt / CREST_HALF_LIFE));
        sh.uniform1f(s_uTexel, 1.f / RES);
        // <WolfViewer 2026-10-04> WAKE STERN: the blur moves foam one TEXEL per pass, and the
        // field is RES texels over the whole region — 0.5 m on 256 m, 6 m on Wolf Territories
        // Home (3,072 m), so foam spread 12x further in metres, forward of the bow too. Scale
        // the rate by (256 / region)^2 so the spread in metres matches a 256 m region.
        const F32 region_m = llmax(256.f, mFieldSize);   // <WolfViewer 2026-10-06/> the field's span
        sh.uniform1f(s_uSpread, llmin(0.6f, 2.2f * dt) * (256.f / region_m) * (256.f / region_m));
        sh.uniform2f(s_uShift, mShiftU, mShiftV);   // <WolfViewer 2026-10-06/> the window moved
        mShiftU = mShiftV = 0.f;
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        sh.unbind();
        dst.flush();
        mCur = 1 - mCur;
    }
    // PASS 2: stamp each boat's path, additively.
    {
        LLGLEnable blend(GL_BLEND);
        gGL.blendFunc(LLRender::BF_ONE, LLRender::BF_ONE);
        S32 budget = MAX_STAMPS_PER_FRAME;
        for (LLViewerObject* obj : boats)
        {
            if (budget <= 0) break;
            budget -= stampBoat(obj, budget);
        }
        gGL.setSceneBlendType(LLRender::BT_ALPHA);
    }
    mReady = true;
}

// Source: wake_field.js WakeField._stampBoat — stamps along the PATH since last frame, so a
// fast boat leaves a continuous trail and a turning one a smooth curve. Speed drives the
// size; the Kelvin angle is a constant the stamp's aspect keeps honest.
S32 WolfWakeField::stampBoat(LLViewerObject* obj, S32 budget)
{
    const LLVector3& v = obj->getVelocity();
    const F32 speed = sqrtf(v.mV[VX] * v.mV[VX] + v.mV[VY] * v.mV[VY]);
    const F32 sp = llclamp((speed - MIN_SPEED) / (FULL_SPEED - MIN_SPEED), 0.f, 1.f);
    const LLVector3 pos = obj->getPositionRegion();
    const F32 x = pos.mV[VX], y = pos.mV[VY];
    BoatState& st = mBoats[obj->getLocalID()];
    if (st.mX == 0.f && st.mY == 0.f)
    {
        st.mX = x;
        st.mY = y;
    }
    F32 dx = x - st.mX, dy = y - st.mY;
    const F32 moved = sqrtf(dx * dx + dy * dy);
    if (moved < 1e-3f)
    {
        dx = v.mV[VX];
        dy = v.mV[VY];
    }
    const F32 dir_len = sqrtf(dx * dx + dy * dy);
    if (dir_len < 1e-6f)
    {
        st.mX = x;
        st.mY = y;
        return 0;
    }
    const F32 ux = dx / dir_len, uy = dy / dir_len;

    const LLVector3& sc = obj->getScale();
    // <WolfViewer 2026-10-04> WAKE STERN (wake_field.js [WAKE STERN 2026-10-04]): the hull's
    // real extent along the heading — the root prim's X and Y sides projected onto it. Paul,
    // on the welcome ferry (73 m) in Wolf Territories Home: "boat wash is going in front of
    // the boat" — the wash started at the hull's CENTRE, 36 m behind the bow, and a 45 m cap
    // sized it for a launch. It now starts at the stern, and only a hull longer than the old
    // cap grows past it, so every boat under 45 m keeps its size.
    // <WolfViewer 2026-10-06> The WHOLE boat, not its root prim: Paul, sailing on autopilot,
    // "there is still wash in front of the boat ... it should only be behind it unless the boat
    // is going backwards". The stern was put half the ROOT prim's length behind the ROOT prim's
    // centre, so a boat whose root sits at the bow (or is a small prim) got its wash mid-hull or
    // ahead of the bow. Every linked prim (not the people sitting on it) is projected onto the
    // direction of travel; the wash starts at the rearmost point, and runs the other way only
    // when the boat itself goes astern (u is the way it is moving).
    F32 fore = -1e9f, aft = 1e9f;
    auto extent = [&](LLViewerObject* p)
    {
        const LLVector3 c = p->getPositionRegion() - pos;
        const LLQuaternion r = p->getRotationRegion();
        const LLVector3& s3 = p->getScale();
        const LLVector3 axes[3] = { LLVector3::x_axis * r, LLVector3::y_axis * r, LLVector3::z_axis * r };
        F32 half = 0.f;
        for (S32 k = 0; k < 3; ++k)
        {
            half += fabsf(ux * axes[k].mV[VX] + uy * axes[k].mV[VY]) * s3.mV[k] * 0.5f;
        }
        const F32 mid = ux * c.mV[VX] + uy * c.mV[VY];
        fore = llmax(fore, mid + half);
        aft = llmin(aft, mid - half);
    };
    extent(obj);
    for (LLViewerObject* child : obj->getChildren())
    {
        if (child && !child->isAvatar())
        {
            extent(child);
        }
    }
    const F32 along = llmax(0.5f, fore - aft);
    const F32 hull_width = llmax(1.f, llmin(llmax(sc.mV[VX], sc.mV[VY]), 30.f));
    const F32 hull_len = llmax(llmax(hull_width, llmin(llmax(sc.mV[VX], sc.mV[VY]) * 1.6f, 45.f)), along);
    const F32 stern_x = ux * aft, stern_y = uy * aft;
    // </WolfViewer>
    const F32 width = hull_width * (1.1f + 2.6f * sp);
    const F32 length = hull_len * (1.2f + 2.8f * sp);
    const S32 segs = llmax(1, llmin(budget, (S32)ceilf(moved / llmax(0.75f, length * 0.4f))));

    static LLStaticHashedString s_uSpeed("uSpeed");
    static LLStaticHashedString s_uFoam("uFoam");
    static LLStaticHashedString s_uAspect("uAspect");
    static LLStaticHashedString s_uKelvinTan("uKelvinTan");
    static LLStaticHashedString s_uCenter("uCenter");
    static LLStaticHashedString s_uAxisX("uAxisX");
    static LLStaticHashedString s_uAxisY("uAxisY");

    LLGLSLShader& sh = gWolfWakeStampProgram;
    LLRenderTarget& dst = mRT[mCur];
    dst.bindTarget();
    sh.bind();
    sh.uniform1f(s_uSpeed, sp);
    sh.uniform1f(s_uAspect, length / llmax(width, 0.001f));
    sh.uniform1f(s_uFoam, 1.f / segs);
    sh.uniform1f(s_uKelvinTan, tanf(asinf(1.f / 3.f)));
    // Field space: 0..1 over the region. Local +Y is "ahead" = the heading; +X is right of
    // it. The quad is slid back by half its length so the wake sits astern of the hull —
    // from the STERN, not the centre (<WolfViewer 2026-10-04/> WAKE STERN).
    const F32 back_x = stern_x - ux * length * 0.5f, back_y = stern_y - uy * length * 0.5f;
    const F32 ax_x = uy * width / mFieldSize, ax_y = -ux * width / mFieldSize;
    const F32 ay_x = ux * length / mFieldSize, ay_y = uy * length / mFieldSize;
    sh.uniform2f(s_uAxisX, ax_x, ax_y);
    sh.uniform2f(s_uAxisY, ay_x, ay_y);
    mQuadVB->setBuffer();
    for (S32 i = 0; i < segs; ++i)
    {
        const F32 t = segs == 1 ? 1.f : (F32)(i + 1) / segs;
        const F32 px = st.mX + dx * t + back_x, py = st.mY + dy * t + back_y;
        sh.uniform2f(s_uCenter, (px - mFieldOX) / mFieldSize, (py - mFieldOY) / mFieldSize);
        mQuadVB->drawRange(LLRender::TRIANGLES, 0, 3, 6, 0);
    }
    sh.unbind();
    dst.flush();

    st.mX = x;
    st.mY = y;
    return segs;
}
