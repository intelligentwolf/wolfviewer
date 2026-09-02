/**
 * @file fstransporterfx.cpp
 * @brief WolfViewer teleport transition — a shower of grains around the avatar.
 *
 * See fstransporterfx.h for what this replaces and why.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Copyright (C) 2026 Wolf Software Systems Ltd
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "fstransporterfx.h"

#include "llagent.h"
#include "llvoavatarself.h"
#include "llviewercamera.h"
#include "llviewerwindow.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llviewershadermgr.h"   // gUIProgram
#include "llcoord.h"
#include "llrand.h"        // ll_frand (llcommon/llrand.h:72,77)
#include "v3color.h"       // LLColor3
#include "v4color.h"       // LLColor4, taken by gl_rect_2d

// Durations. DEMAT matches the web viewer (transporter_fx.js DEMAT_MS); REMAT is LONGER
// than the web viewer's 1400ms deliberately.
//
// [Paul 2026-09-02: "i think it all happens too fast".] Correct, and the reason is
// structural: the beam cannot slow the teleport down, and WolfViewer's teleport is quick, so
// the dematerialise gets cut off within a few frames of starting and everything the user
// actually sees is the rematerialise. That phase is entirely ours — it plays over a region
// that has already arrived — so it is where the time has to go.
static const F32 DEMAT_SECS = 1.8f;
static const F32 REMAT_SECS = 2.4f;

// White, not the reference gold — the same deliberate departure the web viewer makes, so the
// two products look like one. A faint blue bias in the falloff keeps it reading as light
// rather than fog.
static const LLColor3 BEAM_CORE(1.0f, 1.0f, 1.0f);
static const LLColor3 BEAM_EDGE(0.78f, 0.88f, 1.0f);

static const size_t SPARK_MAX = 600;      // fewer than the web build's 2600: these are real geometry
static const size_t SPARK_PER_FRAME = 34;
static const size_t SPARK_SEED = 260;     // pre-seeded so frame one already has grains

FSTransporterFX::FSTransporterFX()
:   mPhase(PHASE_IDLE),
    mLastFrame(0.f),
    mClock(0.f),
    mCentreX(0.f),
    mCentreY(0.f),
    mHeight(0.f),
    mUIScale(1.f),
    mHaveRect(false)
{
}

void FSTransporterFX::start()
{
    if (mPhase == PHASE_DEMAT || mPhase == PHASE_TRANSIT)
    {
        return;                       // already running — a teleport cannot start twice
    }
    mSparks.clear();
    mHaveRect = false;
    updateRect();
    mPhase = PHASE_DEMAT;
    mPhaseTimer.reset();
    mLastFrame = 0.f;
    mClock = 0.f;

    // Seed a body of grains up front. Emitting from zero looks like nothing is happening for
    // the first few frames, which on a fast teleport is most of the phase.
    emit(1.f, (F32)SPARK_SEED / (F32)SPARK_PER_FRAME);

    LL_INFOS("Teleport") << "TransporterFX: start, rect valid=" << (mHaveRect ? 1 : 0)
                         << " x=" << mCentreX << " y=" << mCentreY << " h=" << mHeight
                         << " uiscale=" << mUIScale << LL_ENDL;
}

void FSTransporterFX::finish()
{
    if (mPhase == PHASE_IDLE || mPhase == PHASE_REMAT)
    {
        return;
    }
    updateRect();
    mPhase = PHASE_REMAT;
    mPhaseTimer.reset();
    mLastFrame = 0.f;

    LL_INFOS("Teleport") << "TransporterFX: finish, rect valid=" << (mHaveRect ? 1 : 0)
                         << " x=" << mCentreX << " y=" << mCentreY << " h=" << mHeight
                         << " sparks=" << mSparks.size() << LL_ENDL;
}

void FSTransporterFX::stop()
{
    mPhase = PHASE_IDLE;
    mSparks.clear();
}

/**
 * The avatar's rectangle on screen, in RAW GL pixels.
 *
 * [FIX 2026-09-02, second of the two "it was just a sheet" bugs.]
 * projectPosAgentToScreen returns SCALED (virtual UI) coordinates — llviewercamera.cpp:453
 * divides the projected window coordinate by gViewerWindow->getDisplayScale(). But the plain
 * gl_rect_2d emits vertices with no scaling at all (llrender2dutils.cpp:118), under the ortho
 * that setup2DRender() builds from mWindowRectRaw (llviewerwindow.cpp:6848) — i.e. RAW pixels.
 * So the projection has to be multiplied back up by the display scale or the beam lands at a
 * fraction of the avatar's position on any display where that scale is not 1.
 *
 * The vertical span matches the web viewer exactly (transporter_fx.js _avatarRect: agent
 * position -0.9m to +1.1m), because getRenderPosition() on an avatar returns the drawable
 * ROOT — mid-body, not the feet (llvoavatar.cpp:1454). Measuring 0..+2m from there would put
 * the whole column above the avatar's head.
 *
 * The last good rectangle is KEPT when projection fails, which is what stops the beam jumping
 * to a default the moment the region tears the avatar down mid-teleport.
 */
void FSTransporterFX::updateRect()
{
    if (!isAgentAvatarValid())
    {
        useFallbackRect();
        return;
    }

    const LLVector3 base = gAgentAvatarp->getRenderPosition();
    LLCoordGL feet, head;
    LLViewerCamera& cam = LLViewerCamera::instance();

    // clamp=false: a clamped projection silently reports an off-screen avatar as being at the
    // screen edge, which would plant the beam in the corner. projectPosAgentToScreen returns
    // `in_front && valid` in that mode (llviewercamera.cpp:519), i.e. false whenever the
    // avatar is behind the camera OR outside the world view rect.
    if (!cam.projectPosAgentToScreen(base + LLVector3(0.f, 0.f, -0.9f), feet, false) ||
        !cam.projectPosAgentToScreen(base + LLVector3(0.f, 0.f,  1.1f), head, false))
    {
        useFallbackRect();
        return;
    }

    const LLVector2& scale = gViewerWindow->getDisplayScale();
    const F32 fx = (F32)feet.mX * scale.mV[VX];
    const F32 fy = (F32)feet.mY * scale.mV[VY];
    const F32 hy = (F32)head.mY * scale.mV[VY];
    const F32 win_h = (F32)gViewerWindow->getWindowHeightRaw();

    const F32 h = fabsf(hy - fy);

    // Reject rather than clamp. A 2m span that projects to more than the window height means
    // the camera is inside the avatar — mouselook, or the moment mid-teleport when the two
    // coincide. CLAMPING such a projection is what turns the beam into a full-screen white
    // wash: half_w is derived from the height, so a clamped-huge height gives a column wider
    // and taller than the screen. The old code clamped at 1.2x the window and did exactly that.
    if (!llfinite(fx) || !llfinite(fy) || h < 4.f || h > win_h)
    {
        useFallbackRect();
        return;
    }

    mCentreX  = fx;
    mCentreY  = (fy + hy) * 0.5f;
    mHeight   = h;
    mUIScale  = llmax(0.5f, scale.mV[VY]);
    mHaveRect = true;
}

/**
 * Where the beam goes when the avatar cannot be projected: centred, at a plausible size.
 *
 * Ported from the web viewer's `fallback` (transporter_fx.js _avatarRect), which this port
 * originally dropped. Without it a failed projection left mHaveRect false and NOTHING was
 * drawn at all — and a failed projection is the normal case at the exact moment of a
 * teleport, when the region is tearing the avatar's drawable down.
 */
void FSTransporterFX::useFallbackRect()
{
    const F32 win_h = (F32)gViewerWindow->getWindowHeightRaw();
    const F32 win_w = (F32)gViewerWindow->getWindowWidthRaw();
    const LLVector2& scale = gViewerWindow->getDisplayScale();

    mCentreX  = win_w * 0.5f;
    mCentreY  = win_h * 0.38f;      // web viewer uses 0.62 from the TOP; GL origin is bottom-left
    mHeight   = win_h * 0.34f;
    mUIScale  = llmax(0.5f, scale.mV[VY]);
    mHaveRect = true;
}

void FSTransporterFX::emit(F32 strength, F32 rate)
{
    if (!mHaveRect || mSparks.size() >= SPARK_MAX)
    {
        return;
    }
    const F32 half_w = llmax(8.f, mHeight * 0.26f);
    const size_t n = (size_t)llmax(1.f, (F32)SPARK_PER_FRAME * rate * (0.35f + strength));

    for (size_t i = 0; i < n && mSparks.size() < SPARK_MAX; ++i)
    {
        Spark s;
        // Triangular bias toward the centre line: dense core, sparse edges.
        const F32 u = (ll_frand(1.f) + ll_frand(1.f)) - 1.f;   // transporter_fx.js _emitSparksInColumn
        s.mX    = mCentreX + u * half_w;
        s.mY    = mCentreY + (ll_frand(1.f) - 0.5f) * mHeight * 1.25f;
        s.mVX   = (ll_frand(1.f) - 0.5f) * 14.f * mUIScale;
        s.mVY   = (50.f + ll_frand(180.f)) * mUIScale;   // UP: the original was filmed camera-inverted
        s.mAge  = 0.f;
        s.mLife = 0.35f + ll_frand(0.8f);
        s.mSize = (1.f + ll_frand(2.f)) * mUIScale;
        s.mHot  = (ll_frand(1.f) < 0.30f);
        mSparks.push_back(s);
    }
}

void FSTransporterFX::advance(F32 dt)
{
    for (size_t i = 0; i < mSparks.size(); )
    {
        Spark& s = mSparks[i];
        s.mAge += dt;
        if (s.mAge >= s.mLife)
        {
            mSparks[i] = mSparks.back();
            mSparks.pop_back();
            continue;                 // swap-and-pop: no shuffling a 600-element vector
        }
        s.mX += s.mVX * dt;
        s.mY += s.mVY * dt;
        ++i;
    }
}

/**
 * One grain's glow, as two rings of triangles.
 *
 * The alpha profile is the web viewer's sprite verbatim (transporter_fx.js _sparkSprite):
 * 0.98 at the centre, 0.55 at 28% of the radius, 0 at the rim. That steep inner falloff is
 * the whole character of it — a tight hot glint with a wide, very faint halo. A plain linear
 * ramp instead (which this first had) spreads far too much brightness over the whole disc,
 * and with hundreds of overlapping grains that accumulates straight back into a white wash.
 *
 * NOTE: emits vertices only — the caller must already be inside gGL.begin(LLRender::TRIANGLES),
 * so hundreds of grains batch into a handful of draws instead of one each.
 */
void FSTransporterFX::drawGlow(F32 cx, F32 cy, F32 radius, F32 peak, const LLColor3& col)
{
    if (peak <= 0.004f || radius <= 0.5f)
    {
        return;
    }

    const S32 SEG   = 6;            // the rim is fully transparent, so the hexagon never shows
    const F32 r_mid = radius * 0.28f;
    const F32 a_in  = peak * 0.98f;
    const F32 a_mid = peak * 0.55f;

    for (S32 i = 0; i < SEG; ++i)
    {
        const F32 t0 = F_TWO_PI * (F32)i       / (F32)SEG;
        const F32 t1 = F_TWO_PI * (F32)(i + 1) / (F32)SEG;
        const F32 c0 = cosf(t0), s0 = sinf(t0);
        const F32 c1 = cosf(t1), s1 = sinf(t1);

        // Hot centre out to the mid stop.
        gGL.color4f(col.mV[0], col.mV[1], col.mV[2], a_in);
        gGL.vertex2f(cx, cy);
        gGL.color4f(col.mV[0], col.mV[1], col.mV[2], a_mid);
        gGL.vertex2f(cx + c0 * r_mid, cy + s0 * r_mid);
        gGL.vertex2f(cx + c1 * r_mid, cy + s1 * r_mid);

        // Mid stop fading to nothing at the rim.
        gGL.color4f(col.mV[0], col.mV[1], col.mV[2], a_mid);
        gGL.vertex2f(cx + c0 * r_mid, cy + s0 * r_mid);
        gGL.color4f(col.mV[0], col.mV[1], col.mV[2], 0.f);
        gGL.vertex2f(cx + c0 * radius, cy + s0 * radius);
        gGL.vertex2f(cx + c1 * radius, cy + s1 * radius);

        gGL.color4f(col.mV[0], col.mV[1], col.mV[2], a_mid);
        gGL.vertex2f(cx + c0 * r_mid, cy + s0 * r_mid);
        gGL.color4f(col.mV[0], col.mV[1], col.mV[2], 0.f);
        gGL.vertex2f(cx + c1 * radius, cy + s1 * radius);
        gGL.color4f(col.mV[0], col.mV[1], col.mV[2], a_mid);
        gGL.vertex2f(cx + c1 * r_mid, cy + s1 * r_mid);
    }
}

/**
 * The grains. THE HALO IS THE EFFECT.
 *
 * [Paul 2026-09-02: "it was like a white sheet for just a second not particles".] The first
 * port drew each grain as a bare 1x3 pixel rectangle and nothing else, which at any sane
 * viewing distance is invisible — so all that remained on screen was the column, i.e. a white
 * sheet with no particles in it. The web viewer never draws a bare rectangle: every grain is
 * a soft sprite 6-9x its own size (transporter_fx.js _drawSparks: `halo = s.s * (s.hot ? 9 : 6)`,
 * drawn with the hard core on top). These are backlit metal filings catching the light — the
 * glow is what makes them read as grains rather than dead pixels.
 *
 * The twinkle is from the same place: `tw = 0.65 + 0.35 * sin(now * 0.03 + s.x)`, which is
 * what stops 700 identical dots looking like static.
 */
void FSTransporterFX::drawSparks()
{
    if (mSparks.empty())
    {
        return;
    }

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    // Halos first, cores second, so a grain's own core is never washed out by its neighbour's glow.
    gGL.begin(LLRender::TRIANGLES);
    for (size_t i = 0; i < mSparks.size(); ++i)
    {
        const Spark& s = mSparks[i];
        const F32 k = 1.f - s.mAge / s.mLife;
        if (k <= 0.f)
        {
            continue;
        }
        const F32 tw = 0.65f + 0.35f * sinf(mClock * 30.f + s.mX);
        const LLColor3& base = s.mHot ? BEAM_CORE : BEAM_EDGE;
        drawGlow(s.mX, s.mY, s.mSize * (s.mHot ? 9.f : 6.f), k * tw, base);
    }
    gGL.end();

    gGL.begin(LLRender::TRIANGLES);
    for (size_t i = 0; i < mSparks.size(); ++i)
    {
        const Spark& s = mSparks[i];
        const F32 k = 1.f - s.mAge / s.mLife;
        if (k <= 0.f)
        {
            continue;
        }
        const F32 tw = 0.65f + 0.35f * sinf(mClock * 30.f + s.mX);
        const LLColor3& base = s.mHot ? BEAM_CORE : BEAM_EDGE;
        const F32 a  = k * tw;
        const F32 w  = llmax(1.f, s.mSize * 0.7f);
        const F32 hh = llmax(2.f, s.mSize * 3.2f);      // elongated: a moving grain

        gGL.color4f(base.mV[0], base.mV[1], base.mV[2], a);
        gGL.vertex2f(s.mX,     s.mY + hh);
        gGL.vertex2f(s.mX,     s.mY);
        gGL.vertex2f(s.mX + w, s.mY);

        gGL.vertex2f(s.mX,     s.mY + hh);
        gGL.vertex2f(s.mX + w, s.mY);
        gGL.vertex2f(s.mX + w, s.mY + hh);
    }
    gGL.end();
}

void FSTransporterFX::render()
{
    if (mPhase == PHASE_IDLE)
    {
        return;
    }

    const F32 now = mPhaseTimer.getElapsedTimeF32();
    F32 dt = now - mLastFrame;
    mLastFrame = now;
    // CLAMP, do not replace. Phase progress below is measured against `now` (real time), so
    // substituting a fixed 16ms here made the grains crawl while the phases ran out — on a
    // slow teleport the whole effect would expire with the grains still bunched at the start.
    // Clamping keeps the two roughly in step while still refusing a single huge jump after a
    // stall, which would fling every grain off screen at once.
    dt = llclamp(dt, 0.f, 0.10f);

    // Follow the avatar every frame; updateRect falls back to a centred rectangle whenever the
    // projection fails, which is the normal case while the region is tearing the drawable down.
    updateRect();
    mClock += dt;

    switch (mPhase)
    {
        case PHASE_DEMAT:
        {
            const F32 t = llmin(1.f, now / DEMAT_SECS);
            emit(t, 1.f);
            if (t >= 1.f)
            {
                mPhase = PHASE_TRANSIT;
                mPhaseTimer.reset();
                mLastFrame = 0.f;
            }
            break;
        }
        case PHASE_TRANSIT:
            emit(0.5f, 0.25f);        // a residual shimmer where they were
            break;

        case PHASE_REMAT:
        {
            const F32 t = llmin(1.f, now / REMAT_SECS);
            emit(1.f - t, 1.f);      // thinning out as they solidify
            if (t >= 1.f)
            {
                stop();
                return;
            }
            break;
        }
        default:
            break;
    }

    advance(dt);

    // A SHADER MUST BE BOUND BEFORE ANY DRAW.
    //
    // [FIX 2026-09-02] This crashed the viewer on the first teleport:
    //   ASSERT (LLGLSLShader::sCurBoundShaderPtr != nullptr)  llrender.cpp(1614) flush
    // The viewer is core-profile, so every draw needs a bound GLSL program, and gl_rect_2d
    // does not bind one — it assumes the caller has. render_ui_2d() only binds gUIProgram
    // further down, AFTER the point this effect draws at, so at this moment nothing is
    // bound and the first flush trips the assert.
    //
    // Bind it ourselves, flush while it is still bound (unbinding with geometry queued
    // would hit the same assert on the next flush), then put the state back exactly as it
    // was found — unbound, alpha blending — because the code that follows in render_ui_2d
    // binds its own shaders and expects no inherited state.
    gUIProgram.bind();
    // BT_ADD_WITH_ALPHA, *NOT* BT_ADD.
    //
    // [FIX 2026-09-02] BT_ADD is blendFunc(BF_ONE, BF_ONE) (llrender.cpp setSceneBlendType) —
    // source alpha is not a factor, so every fragment adds full-strength white and every
    // alpha gradient in this file was silently discarded. That is what produced a flat white
    // rectangle with hard edges and solid white hexagons instead of soft grains.
    // BT_ADD_WITH_ALPHA is blendFunc(BF_SOURCE_ALPHA, BF_ONE), the true equivalent of the
    // canvas globalCompositeOperation='lighter' the web viewer composites with.
    gGL.setSceneBlendType(LLRender::BT_ADD_WITH_ALPHA);

    // LLRender applies the UI offset/scale stack to every vertex (llrender.cpp:1837
    // transform(), called from vertex3f). Our coordinates are already raw window pixels, so
    // anything left on that stack would move the beam. gl_rect_2d_offset_local does the same
    // thing for the same reason.
    gGL.pushUIMatrix();
    gGL.loadUIIdentity();

    drawSparks();

    // Flush while the shader is still bound: unbinding with geometry queued would trip the
    // very assert this call is here to avoid.
    gGL.flush();
    gGL.popUIMatrix();
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gUIProgram.unbind();
}
