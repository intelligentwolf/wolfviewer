/**
 * @file wolffx.cpp
 * @brief WolfViewer: script effects (explosions and the rest) and camera shake. See wolffx.h.
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

#include "wolffx.h"

#include <cmath>
#include <cstdlib>

#include "llagent.h"
#include "llmutelist.h"
#include "lldispatcher.h"
#include "llrand.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewergenericmessage.h"   // gGenericDispatcher
#include "llviewerpartsim.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llvoavatar.h"           // LLViewerPartSource holds an LLPointer<LLVOAvatar> (as wolflightning.cpp)
#include "llworld.h"
#include "wolfcamerafx.h"

namespace
{
    const std::string MESSAGE_FROM_SIM("WolfFX");     // WolfFXModule.cs METHOD_OUT
    const F32 DRAW_DISTANCE = 512.f;                  // effects further than this from the camera are not drawn
    const F32 FX_GRAVITY = 9.81f;

    F32 rnd(F32 a, F32 b) { return a + ll_frand(b - a); }

    LLVector3 rnd_unit()
    {
        // uniform on the sphere (Marsaglia)
        F32 x, y, s;
        do { x = rnd(-1.f, 1.f); y = rnd(-1.f, 1.f); s = x * x + y * y; } while (s >= 1.f || s < 1e-6f);
        const F32 k = 2.f * sqrtf(1.f - s);
        return LLVector3(x * k, y * k, 1.f - 2.f * s);
    }

    /** A random direction within `cone` radians of `axis`. */
    LLVector3 rnd_cone(const LLVector3& axis, F32 cone)
    {
        LLVector3 a = axis;
        a.normalize();
        for (S32 tries = 0; tries < 16; ++tries)
        {
            const LLVector3 v = rnd_unit();
            if (acosf(llclamp(v * a, -1.f, 1.f)) <= cone) return v;
        }
        return a;
    }

    LLColor4 mix(const LLColor4& a, const LLColor4& b, F32 t)
    {
        return LLColor4(a.mV[0] + (b.mV[0] - a.mV[0]) * t, a.mV[1] + (b.mV[1] - a.mV[1]) * t,
                        a.mV[2] + (b.mV[2] - a.mV[2]) * t, a.mV[3] + (b.mV[3] - a.mV[3]) * t);
    }

    LLColor4 alpha(const LLColor4& c, F32 a) { return LLColor4(c.mV[0], c.mV[1], c.mV[2], a); }

    bool parse_vec(const std::string& s, LLVector3& out)
    {
        F32 x, y, z;
        if (sscanf(s.c_str(), "%f,%f,%f", &x, &y, &z) != 3) return false;
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
        out.set(x, y, z);
        return true;
    }

    class WolfFXHandler : public LLDispatchHandler
    {
    public:
        bool operator()(const LLDispatcher*, const std::string&, const LLUUID& invoice, const sparam_t& strings) override
        {
            WolfFX::instance().receive(invoice, strings);
            return true;
        }
    };
    WolfFXHandler sHandler;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════
// WolfFX
// ═══════════════════════════════════════════════════════════════════════════════════════════════

WolfFX::WolfFX()
{
    if (!gGenericDispatcher.isHandlerPresent(MESSAGE_FROM_SIM))
    {
        gGenericDispatcher.addHandler(MESSAGE_FROM_SIM, &sHandler);
    }
}

WolfFX::~WolfFX()
{
}

void WolfFX::idle()
{
    for (auto it = mSources.begin(); it != mSources.end();)
    {
        if ((*it)->isDead() || (*it)->finished())
        {
            if (!(*it)->isDead()) (*it)->setDead();
            it = mSources.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void WolfFX::receive(const LLUUID& region_id, const std::vector<std::string>& strings)
{
    std::string e;
    LLVector3 p, c(1.f, 0.55f, 0.15f), c2(0.25f, 0.24f, 0.23f), dir(0.f, 0.f, 1.f);
    F32 size = 4.f, duration = 1.f, intensity = 1.f, shake = 0.f, shake_r = 30.f, amp = 0.f;
    bool have_p = false;
    LLUUID owner;
    for (const std::string& kv : strings)
    {
        const size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
        F32 f = (F32)atof(v.c_str());
        if (!std::isfinite(f)) f = 0.f;     // [SECURITY 2026-10-10] a NaN would pass llclamp
        if (k == "e") e = v;
        else if (k == "p") have_p = parse_vec(v, p);
        else if (k == "s") size = llclamp(f, 0.2f, 30.f);
        else if (k == "c") parse_vec(v, c);
        else if (k == "c2") parse_vec(v, c2);
        else if (k == "d") duration = llclamp(f, 0.05f, 30.f);
        else if (k == "i") intensity = llclamp(f, 0.1f, 2.f);
        else if (k == "sh") shake = llclamp(f, 0.f, 1.f);
        else if (k == "sr") shake_r = llclamp(f, 0.f, 150.f);
        else if (k == "dir") parse_vec(v, dir);
        else if (k == "a") amp = llclamp(f, 0.f, 1.f);
        else if (k == "o") owner.set(v, false);
    }
    // Nothing from someone this viewer has muted (WolfFXModule sends the object's owner).
    if (owner.notNull() && LLMuteList::getInstance()->isMuted(owner)) return;

    static LLCachedControl<bool> shake_on(gSavedSettings, "WolfFXCameraShake", true);
    if (e == "shake")
    {
        if (shake_on) WolfCameraFX::instance().shake(amp * 0.6f, duration);
        return;
    }

    static LLCachedControl<bool> fx_on(gSavedSettings, "WolfFXEnabled", true);
    LLViewerRegion* region = LLWorld::getInstance()->getRegionFromID(region_id);
    if (!region || !have_p) return;
    const LLVector3d global = region->getPosGlobalFromRegion(p);

    WolfFXSource::EKind kind;
    if (e == "explosion") kind = WolfFXSource::EXPLOSION;
    else if (e == "fireball") kind = WolfFXSource::FIREBALL;
    else if (e == "smoke") kind = WolfFXSource::SMOKE;
    else if (e == "sparks") kind = WolfFXSource::SPARKS;
    else if (e == "fire") kind = WolfFXSource::FIRE;
    else if (e == "fireworks") kind = WolfFXSource::FIREWORKS;
    else if (e == "dust") kind = WolfFXSource::DUST;
    else if (e == "shockwave") kind = WolfFXSource::SHOCKWAVE;
    else return;    // a newer simulator's effect this viewer does not know

    // The shake: strongest at the centre, gone at shake_r, falling with the square of the distance
    // (an explosion's pressure falls fast); felt from where the camera is.
    if (shake_on && shake > 0.f && shake_r > 0.f)
    {
        const F32 d = (F32)(gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin()) - global).magVec();
        const F32 k = llclamp(1.f - d / shake_r, 0.f, 1.f);
        if (k > 0.f) WolfCameraFX::instance().shake(shake * k * k * llclamp(size / 4.f, 0.4f, 2.f) * 0.5f, 0.5f + size * 0.08f);
    }

    if (!fx_on) return;
    const LLVector3 agent = gAgent.getPosAgentFromGlobal(global);
    if ((agent - LLViewerCamera::getInstance()->getOrigin()).length() > DRAW_DISTANCE) return;
    if (mSources.size() >= 48) return;    // a flood of effects: the newest wait their turn

    const F32 dur = (kind == WolfFXSource::SMOKE || kind == WolfFXSource::FIRE || kind == WolfFXSource::SPARKS) ? duration : llmax(duration, 0.5f);
    LLPointer<WolfFXSource> src = new WolfFXSource(kind, global, size, LLColor4(c.mV[0], c.mV[1], c.mV[2], 1.f),
                                                   LLColor4(c2.mV[0], c2.mV[1], c2.mV[2], 1.f), dur, intensity, dir);
    LLViewerPartSim::getInstance()->addPartSource(src);
    mSources.push_back(src);
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════
// WolfFXSource
// ═══════════════════════════════════════════════════════════════════════════════════════════════

WolfFXSource::WolfFXSource(EKind kind, const LLVector3d& global, F32 size, const LLColor4& c, const LLColor4& c2,
                           F32 duration, F32 intensity, const LLVector3& dir)
    : LLViewerPartSource(LL_PART_SOURCE_NULL),   // as WolfLightningPartSource (wolflightning.cpp)
      mKind(kind), mGlobal(global), mSize(size), mDuration(duration), mIntensity(intensity), mC(c), mC2(c2), mDir(dir)
{
    if (mDir.normalize() < 1e-4f) mDir = LLVector3::z_axis;
}

S32 WolfFXSource::count(F32 n) const
{
    static LLCachedControl<S32> max_parts(gSavedSettings, "RenderMaxPartCount", 4096);
    // Fewer when the viewer's particle budget is small (the Graphics preference).
    const F32 budget = llclamp((F32)(S32)max_parts / 4096.f, 0.25f, 1.f);
    return llmax(1, (S32)llround(n * mIntensity * budget));
}

LLViewerPart* WolfFXSource::part(const LLVector3& pos, const LLVector3& vel, const LLVector3& accel, F32 life,
                                 F32 scale0, F32 scale1, const LLColor4& c0, const LLColor4& c1, F32 glow0, F32 glow1,
                                 U32 flags, bool additive)
{
    if (!LLViewerPartSim::shouldAddPart()) return nullptr;
    LLViewerPart* p = new LLViewerPart();
    p->init(this, mImagep, NULL);
    p->mBlendFuncSource = LLRender::BF_SOURCE_ALPHA;
    p->mBlendFuncDest = additive ? LLRender::BF_ONE : LLRender::BF_ONE_MINUS_SOURCE_ALPHA;
    p->mLastUpdateTime = 0.f;
    p->mPosAgent = pos;
    p->mVelocity = vel;
    p->mAccel = accel;
    p->mMaxAge = life;
    p->mScale.set(scale0, scale0);
    p->mStartScale = p->mScale;
    p->mEndScale.set(scale1, scale1);
    p->mStartColor = c0;
    p->mEndColor = c1;
    p->mColor = c0;
    p->mStartGlow = glow0;
    p->mEndGlow = glow1;
    p->mGlow = LLColor4U(0, 0, 0, (U8)(llclamp(glow0, 0.f, 1.f) * 255.f));
    p->mFlags = flags | LLViewerPart::LL_PART_INTERP_COLOR_MASK | LLViewerPart::LL_PART_INTERP_SCALE_MASK;
    p->mParameter = 0.f;
    LLViewerPartSim::getInstance()->addPart(p);
    return p;
}

void WolfFXSource::update(const F32 dt)
{
    if (!mImagep)
    {
        mImagep = LLViewerFetchedTexture::sDefaultParticleImagep;   // the soft round particle every viewer has
    }
    // Agent space moves when the agent changes region: from the global position every frame. The
    // source stands on the ground under the effect (the bounce plane for sparks and debris).
    mCentre = gAgent.getPosAgentFromGlobal(mGlobal);
    const F32 ground = LLWorld::getInstance()->resolveLandHeightAgent(mCentre);
    mPosAgent = LLVector3(mCentre.mV[VX], mCentre.mV[VY], llmin(mCentre.mV[VZ], ground));
    if (!mBurst)
    {
        mBurst = true;
        burst();
    }
    mAge += dt;
    if (mAge <= mDuration) stream(dt);
}

// The first moment of each effect. Sizes scale with the radius R; counts with the intensity.
void WolfFXSource::burst()
{
    const F32 R = mSize;
    const LLVector3 c = mCentre;
    const LLVector3 g(0.f, 0.f, -FX_GRAVITY);
    const U32 EMISSIVE = LLViewerPart::LL_PART_EMISSIVE_MASK;
    const U32 STREAK = LLViewerPart::LL_PART_FOLLOW_VELOCITY_MASK | LLViewerPart::LL_PART_EMISSIVE_MASK;
    const U32 BOUNCE = LLViewerPart::LL_PART_BOUNCE_MASK;
    const LLColor4 hot(1.f, 0.96f, 0.82f, 1.f);

    auto flash = [&](F32 scale, F32 life, F32 a)
    {
        for (S32 i = 0; i < 3; ++i)
            part(c, LLVector3::zero, LLVector3::zero, life, scale * 0.7f, scale, alpha(hot, a), LLColor4(1.f, 0.6f, 0.2f, 0.f), 1.f, 0.f, EMISSIVE, true);
    };
    auto fireball = [&](S32 n, F32 speed)
    {
        for (S32 i = 0; i < n; ++i)
        {
            LLVector3 d = rnd_unit();
            d.mV[VZ] = fabsf(d.mV[VZ]) * 0.8f + 0.2f;     // more up than down: the ground is in the way
            d.normalize();
            const F32 sp = R * rnd(speed * 0.4f, speed);
            const LLColor4 start = mix(hot, mC, rnd(0.2f, 0.7f));
            const LLColor4 end(mC.mV[0] * 0.55f + 0.1f, mC.mV[1] * 0.2f, mC.mV[2] * 0.1f, 0.f);
            part(c + d * (R * 0.15f), d * sp, -d * sp * 1.1f + LLVector3(0.f, 0.f, R * 0.6f), rnd(0.6f, 1.2f),
                 R * rnd(0.3f, 0.45f), R * rnd(0.9f, 1.3f), start, end, 0.7f, 0.f, EMISSIVE, true);
        }
    };
    auto sparks = [&](S32 n, F32 speed, const LLVector3& axis, F32 cone)
    {
        for (S32 i = 0; i < n; ++i)
        {
            const LLVector3 d = rnd_cone(axis, cone);
            const F32 sz = llclamp(R * 0.05f, 0.05f, 0.35f);
            part(c, d * rnd(speed * 0.5f, speed), g, rnd(0.8f, 2.2f), sz, sz * 0.5f,
                 LLColor4(1.f, 0.88f, 0.45f, 1.f), LLColor4(1.f, 0.3f, 0.05f, 0.f), 0.6f, 0.f, STREAK | BOUNCE, true);
        }
    };
    auto smoke = [&](S32 n, F32 rise, F32 life)
    {
        for (S32 i = 0; i < n; ++i)
        {
            const LLVector3 off = rnd_unit() * (R * rnd(0.f, 0.6f));
            const LLVector3 v(rnd(-0.4f, 0.4f) * R * 0.3f, rnd(-0.4f, 0.4f) * R * 0.3f, rise * rnd(0.6f, 1.2f));
            const F32 shade = rnd(0.7f, 1.1f);
            part(c + off, v, LLVector3(0.f, 0.f, rise * 0.1f), life * rnd(0.7f, 1.2f), R * rnd(0.6f, 0.9f), R * rnd(2.2f, 3.2f),
                 LLColor4(mC2.mV[0] * shade, mC2.mV[1] * shade, mC2.mV[2] * shade, 0.55f),
                 LLColor4(mC2.mV[0] * 0.6f, mC2.mV[1] * 0.6f, mC2.mV[2] * 0.6f, 0.f), 0.f, 0.f, 0, false);
        }
    };
    auto ring = [&](S32 n, F32 speed, F32 a)
    {
        for (S32 i = 0; i < n; ++i)
        {
            const F32 ang = 6.2831853f * (F32)i / (F32)n;
            const LLVector3 d(cosf(ang), sinf(ang), 0.f);
            part(LLVector3(c.mV[VX], c.mV[VY], mPosAgent.mV[VZ] + 0.3f) + d * (R * 0.3f), d * (R * speed), -d * (R * speed * 1.4f), 0.45f,
                 R * 0.5f, R * 1.1f, LLColor4(1.f, 1.f, 1.f, a), LLColor4(0.9f, 0.9f, 0.85f, 0.f), 0.f, 0.f, 0, false);
        }
    };

    switch (mKind)
    {
    case EXPLOSION:
        flash(R * 4.f, 0.18f, 1.f);
        fireball(count(40), 3.5f);
        ring(count(36), 10.f, 0.3f);
        sparks(count(60), R * 2.5f + 6.f, LLVector3::z_axis, 1.4f);
        // debris: dark chunks on arcs that bounce
        for (S32 i = 0, n = count(18); i < n; ++i)
        {
            const LLVector3 d = rnd_cone(LLVector3::z_axis, 1.1f);
            const F32 sz = llclamp(R * rnd(0.04f, 0.1f), 0.06f, 0.6f);
            part(c, d * rnd(R * 1.5f, R * 3.f + 4.f), LLVector3(0.f, 0.f, -FX_GRAVITY), rnd(1.4f, 3.f), sz, sz,
                 alpha(mC2 * 0.6f, 1.f), alpha(mC2 * 0.4f, 0.f), 0.f, 0.f, BOUNCE, false);
        }
        smoke(count(26), R * 0.45f, 6.f);
        break;
    case FIREBALL:
        flash(R * 2.5f, 0.12f, 0.8f);
        fireball(count(36), 2.5f);
        smoke(count(10), R * 0.35f, 4.f);
        break;
    case SPARKS:
        sparks(count(40), 4.f + R * 3.f, mDir, 0.6f);
        break;
    case FIREWORKS:
    {
        flash(R * 0.8f, 0.15f, 0.9f);
        for (S32 i = 0, n = count(140); i < n; ++i)
        {
            const LLVector3 d = rnd_unit();
            const LLColor4 col = (i % 3 == 0) ? mC2 : mC;
            const F32 sz = llclamp(R * 0.035f, 0.08f, 0.6f);
            part(c, d * (R * rnd(0.85f, 1.05f) / 0.9f), LLVector3(0.f, 0.f, -2.f), rnd(1.6f, 2.4f), sz, sz * 0.4f,
                 alpha(col, 1.f), alpha(col * 0.8f, 0.f), 0.8f, 0.1f, STREAK, true);
        }
        break;
    }
    case DUST:
        ring(count(30), 2.5f, 0.0f);
        for (S32 i = 0, n = count(28); i < n; ++i)
        {
            const F32 ang = ll_frand(6.2831853f);
            const LLVector3 d(cosf(ang), sinf(ang), rnd(0.05f, 0.35f));
            part(LLVector3(c.mV[VX], c.mV[VY], mPosAgent.mV[VZ] + 0.2f), d * (R * rnd(0.6f, 1.4f)), -d * (R * 0.35f), rnd(2.f, 3.5f),
                 R * 0.4f, R * rnd(1.2f, 1.8f), alpha(mC, 0.5f), alpha(mC * 0.9f, 0.f), 0.f, 0.f, 0, false);
        }
        break;
    case SHOCKWAVE:
        flash(R * 0.8f, 0.1f, 0.5f);
        ring(count(48), 12.f, 0.45f);
        break;
    case SMOKE:
    case FIRE:
        break;          // all streamed
    }
}

// What keeps coming while the effect lasts.
void WolfFXSource::stream(F32 dt)
{
    const F32 R = mSize;
    const LLVector3 c = mCentre;
    F32 rate = 0.f;     // particles a second at intensity 1
    switch (mKind)
    {
    case SMOKE: rate = 10.f; break;
    case FIRE: rate = 36.f; break;
    case SPARKS: rate = 30.f; break;
    default: return;
    }
    mCarry += rate * mIntensity * dt;
    S32 n = (S32)mCarry;
    mCarry -= (F32)n;
    // Thin out at the very end so it does not stop dead.
    const F32 fade = llclamp((mDuration - mAge) / 0.5f, 0.f, 1.f);
    for (S32 i = 0; i < n; ++i)
    {
        if (ll_frand() > fade) continue;
        switch (mKind)
        {
        case SMOKE:
        {
            const LLVector3 off(rnd(-0.5f, 0.5f) * R, rnd(-0.5f, 0.5f) * R, 0.f);
            const F32 shade = rnd(0.75f, 1.1f);
            part(c + off, LLVector3(rnd(-0.3f, 0.3f), rnd(-0.3f, 0.3f), R * rnd(0.4f, 0.8f)), LLVector3(0.f, 0.f, 0.2f), rnd(4.f, 7.f),
                 R * 0.6f, R * rnd(2.f, 3.f), LLColor4(mC.mV[0] * shade, mC.mV[1] * shade, mC.mV[2] * shade, 0.5f), alpha(mC * 0.7f, 0.f),
                 0.f, 0.f, 0, false);
            break;
        }
        case FIRE:
        {
            // flames: rise and shrink, yellow-white core to orange to dark red
            const LLVector3 off(rnd(-0.4f, 0.4f) * R, rnd(-0.4f, 0.4f) * R, rnd(0.f, 0.2f) * R);
            part(c + off, mDir * rnd(0.5f, 1.2f) + LLVector3(rnd(-0.2f, 0.2f), rnd(-0.2f, 0.2f), 0.f), mDir * (R * 2.5f), rnd(0.5f, 0.9f),
                 R * rnd(0.45f, 0.7f), R * 0.12f, mix(LLColor4(1.f, 0.95f, 0.7f, 0.9f), alpha(mC, 0.9f), rnd(0.f, 0.5f)),
                 LLColor4(mC.mV[0] * 0.6f + 0.2f, mC.mV[1] * 0.15f, 0.f, 0.f), 0.5f, 0.f, LLViewerPart::LL_PART_EMISSIVE_MASK, true);
            if (ll_frand() < 0.08f)     // an ember
                part(c + off, mDir * rnd(2.f, 4.f) + rnd_unit(), LLVector3(0.f, 0.f, -1.f), rnd(1.f, 2.f), 0.06f, 0.03f,
                     LLColor4(1.f, 0.7f, 0.3f, 1.f), LLColor4(1.f, 0.2f, 0.f, 0.f), 0.6f, 0.f, LLViewerPart::LL_PART_EMISSIVE_MASK, true);
            if (ll_frand() < 0.12f)     // smoke above the flames
                part(c + off + mDir * (R * 1.2f), mDir * rnd(0.8f, 1.6f), LLVector3::zero, rnd(3.f, 5.f), R * 0.6f, R * 2.f,
                     LLColor4(0.22f, 0.21f, 0.2f, 0.35f), LLColor4(0.3f, 0.3f, 0.3f, 0.f), 0.f, 0.f, 0, false);
            break;
        }
        case SPARKS:
        {
            const LLVector3 d = rnd_cone(mDir, 0.5f);
            const F32 sz = llclamp(R * 0.05f, 0.05f, 0.3f);
            part(c, d * rnd(3.f, 6.f + R * 3.f), LLVector3(0.f, 0.f, -FX_GRAVITY), rnd(0.6f, 1.6f), sz, sz * 0.5f,
                 LLColor4(1.f, 0.9f, 0.5f, 1.f), LLColor4(1.f, 0.35f, 0.05f, 0.f), 0.6f, 0.f,
                 LLViewerPart::LL_PART_FOLLOW_VELOCITY_MASK | LLViewerPart::LL_PART_EMISSIVE_MASK | LLViewerPart::LL_PART_BOUNCE_MASK, true);
            break;
        }
        default:
            break;
        }
    }
}
