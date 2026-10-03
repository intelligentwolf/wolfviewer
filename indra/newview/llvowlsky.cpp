/**
 * @file llvowlsky.cpp
 * @brief LLVOWLSky class implementation
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
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

#include "llviewerprecompiledheaders.h"

#include "pipeline.h"

#include "llvowlsky.h"
#include "llsky.h"
#include "lldrawpoolwlsky.h"
#include "llface.h"
#include "llviewercontrol.h"
#include "llenvironment.h"
#include "llsettingssky.h"
#include "llframetimer.h"
#include "lltimer.h"
#include "wolfregionweather.h"   // <WolfViewer 2026-10-03/> real star map on automatic regions
#include "llhudtext.h"
#include "llhudobject.h"
#include "llfontgl.h"
#include "llviewercamera.h"
#include <fstream>
#include <sstream>

constexpr U32 MIN_SKY_DETAIL = 8;
constexpr U32 MAX_SKY_DETAIL = 180;

inline U32 LLVOWLSky::getNumStacks(void)
{
    return llmin(MAX_SKY_DETAIL, llmax(MIN_SKY_DETAIL, gSavedSettings.getU32("WLSkyDetail")));
}

inline U32 LLVOWLSky::getNumSlices(void)
{
    return 2 * llmin(MAX_SKY_DETAIL, llmax(MIN_SKY_DETAIL, gSavedSettings.getU32("WLSkyDetail")));
}

inline U32 LLVOWLSky::getStripsNumVerts(void)
{
    return (getNumStacks() - 1) * getNumSlices();
}

inline U32 LLVOWLSky::getStripsNumIndices(void)
{
    return 2 * ((getNumStacks() - 2) * (getNumSlices() + 1)) + 1 ;
}

inline U32 LLVOWLSky::getStarsNumVerts(void)
{
    return 1000;
}

inline U32 LLVOWLSky::getStarsNumIndices(void)
{
    return 1000;
}

LLVOWLSky::LLVOWLSky(const LLUUID &id, const LLPCode pcode, LLViewerRegion *regionp)
    : LLStaticViewerObject(id, pcode, regionp, true)
{
    initStars();
}

void LLVOWLSky::idleUpdate(LLAgent &agent, const F64 &time)
{

}

bool LLVOWLSky::isActive(void) const
{
    return false;
}

LLDrawable * LLVOWLSky::createDrawable(LLPipeline * pipeline)
{
    pipeline->allocDrawable(this);

    //LLDrawPoolWLSky *poolp = static_cast<LLDrawPoolWLSky *>(
        gPipeline.getPool(LLDrawPool::POOL_WL_SKY);

    mDrawable->setRenderType(LLPipeline::RENDER_TYPE_WL_SKY);

    return mDrawable;
}

// a tiny helper function for controlling the sky dome tesselation.
inline F32 calcPhi(const U32 &i, const F32 &reciprocal_num_stacks)
{
    // Calc: PI/8 * 1-((1-t^4)*(1-t^4))  { 0<t<1 }
    // Demos: \pi/8*\left(1-((1-x^{4})*(1-x^{4}))\right)\ \left\{0<x\le1\right\}

    // i should range from [0..SKY_STACKS] so t will range from [0.f .. 1.f]
    F32 t = float(i) * reciprocal_num_stacks; //SL-16127: remove: / float(getNumStacks());

    // ^4 the parameter of the tesselation to bias things toward 0 (the dome's apex)
    t *= t;
    t *= t;

    // invert and square the parameter of the tesselation to bias things toward 1 (the horizon)
    t = 1.f - t;
    t = t*t;
    t = 1.f - t;

    return (F_PI / 8.f) * t;
}

void LLVOWLSky::resetVertexBuffers()
{
    mStripsVerts.clear();
    mStarsVerts = nullptr;
    mRealStarsVerts = nullptr;   // <WolfViewer 2026-10-03/>
    mFsSkyVerts = nullptr;

    gPipeline.markRebuild(mDrawable, LLDrawable::REBUILD_ALL);
}

void LLVOWLSky::cleanupGL()
{
    mStripsVerts.clear();
    mStarsVerts = nullptr;
    mRealStarsVerts = nullptr;   // <WolfViewer 2026-10-03/>
    mFsSkyVerts = nullptr;

    LLDrawPoolWLSky::cleanupGL();
}

void LLVOWLSky::restoreGL()
{
    LLDrawPoolWLSky::restoreGL();
    gPipeline.markRebuild(mDrawable, LLDrawable::REBUILD_ALL);
}

bool LLVOWLSky::updateGeometry(LLDrawable * drawable)
{
    LL_PROFILE_ZONE_SCOPED;
    LLStrider<LLVector3>    vertices;
    LLStrider<LLVector2>    texCoords;
    LLStrider<U16>          indices;

    if (mFsSkyVerts.isNull())
    {
        mFsSkyVerts = new LLVertexBuffer(LLDrawPoolWLSky::ADV_ATMO_SKY_VERTEX_DATA_MASK);

        if (!mFsSkyVerts->allocateBuffer(4, 6))
        {
            LL_WARNS() << "Failed to allocate Vertex Buffer on full screen sky update" << LL_ENDL;
        }

        bool success = mFsSkyVerts->getVertexStrider(vertices)
                    && mFsSkyVerts->getTexCoord0Strider(texCoords)
                    && mFsSkyVerts->getIndexStrider(indices);

        if(!success)
        {
            LL_ERRS() << "Failed updating WindLight fullscreen sky geometry." << LL_ENDL;
        }

        *vertices++ = LLVector3(-1.0f, -1.0f, 0.0f);
        *vertices++ = LLVector3( 1.0f, -1.0f, 0.0f);
        *vertices++ = LLVector3(-1.0f,  1.0f, 0.0f);
        *vertices++ = LLVector3( 1.0f,  1.0f, 0.0f);

        *texCoords++ = LLVector2(0.0f, 0.0f);
        *texCoords++ = LLVector2(1.0f, 0.0f);
        *texCoords++ = LLVector2(0.0f, 1.0f);
        *texCoords++ = LLVector2(1.0f, 1.0f);

        *indices++ = 0;
        *indices++ = 1;
        *indices++ = 2;
        *indices++ = 1;
        *indices++ = 3;
        *indices++ = 2;

        mFsSkyVerts->unmapBuffer();
    }

    {
        const F32 dome_radius = LLEnvironment::instance().getCurrentSky()->getDomeRadius();
        LLCachedControl<S32> max_vbo_size(gSavedSettings, "RenderMaxVBOSize", 512);
        const U32 max_buffer_bytes = max_vbo_size * 1024;
        const U32 data_mask = LLDrawPoolWLSky::SKY_VERTEX_DATA_MASK;
        const U32 max_verts = max_buffer_bytes / LLVertexBuffer::calcVertexSize(data_mask);

        const U32 total_stacks = getNumStacks();

        const U32 verts_per_stack = getNumSlices();

        // each seg has to have one more row of verts than it has stacks
        // then round down
        const U32 stacks_per_seg = (max_verts - verts_per_stack) / verts_per_stack;

        // round up to a whole number of segments
        const U32 strips_segments = (total_stacks+stacks_per_seg-1) / stacks_per_seg;

        mStripsVerts.resize(strips_segments, NULL);

#if RELEASE_SHOW_DEBUG
        LL_INFOS() << "WL Skydome strips in " << strips_segments << " batches." << LL_ENDL;

        LLTimer timer;
        timer.start();
#endif

        for (U32 i = 0; i < strips_segments ;++i)
        {
            LLVertexBuffer * segment = new LLVertexBuffer(LLDrawPoolWLSky::SKY_VERTEX_DATA_MASK);
            mStripsVerts[i] = segment;

            U32 num_stacks_this_seg = stacks_per_seg;
            if ((i == strips_segments - 1) && (total_stacks % stacks_per_seg) != 0)
            {
                // for the last buffer only allocate what we'll use
                num_stacks_this_seg = total_stacks % stacks_per_seg;
            }

            // figure out what range of the sky we're filling
            const U32 begin_stack = i * stacks_per_seg;
            const U32 end_stack = begin_stack + num_stacks_this_seg;
            llassert(end_stack <= total_stacks);

            const U32 num_verts_this_seg = verts_per_stack * (num_stacks_this_seg+1);
            llassert(num_verts_this_seg <= max_verts);

            const U32 num_indices_this_seg = 1+num_stacks_this_seg*(2+2*verts_per_stack);
            llassert(num_indices_this_seg * sizeof(U16) <= max_buffer_bytes);

            bool allocated = segment->allocateBuffer(num_verts_this_seg, num_indices_this_seg);
#if RELEASE_SHOW_WARNS
            if( !allocated )
            {
                LL_WARNS() << "Failed to allocate Vertex Buffer on update to "
                    << num_verts_this_seg << " vertices and "
                    << num_indices_this_seg << " indices" << LL_ENDL;
            }
#else
            (void) allocated;
#endif

            // lock the buffer
            bool success = segment->getVertexStrider(vertices)
                && segment->getTexCoord0Strider(texCoords)
                && segment->getIndexStrider(indices);

#if RELEASE_SHOW_DEBUG
            if(!success)
            {
                LL_ERRS() << "Failed updating WindLight sky geometry." << LL_ENDL;
            }
#else
            (void) success;
#endif

            // fill it
            buildStripsBuffer(begin_stack, end_stack, vertices, texCoords, indices, dome_radius, verts_per_stack, total_stacks);

            // and unlock the buffer
            segment->unmapBuffer();
        }

#if RELEASE_SHOW_DEBUG
        LL_INFOS() << "completed in " << llformat("%.2f", timer.getElapsedTimeF32().value()) << "seconds" << LL_ENDL;
#endif
    }

    updateStarColors();
    updateStarGeometry(drawable);

    LLPipeline::sCompiles++;

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// <WolfViewer 2026-10-03> The real star map (Jimmy Olsen's Automatic Environment).
// On a region following a real place, the random star field is replaced by the 5,080 naked-eye
// stars of the Yale Bright Star Catalogue (CDS V/50, V <= 6.0, J2000; generated by
// region-terrain-work/autoenv-stars/make_stars.py) where they really are in that place's sky now.
// Equatorial -> horizon: local sidereal time = GMST (Meeus eq. 12.4) + longitude; then, in the
// agent frame (+x east, +y north, +z up, as the sun and moon), with hour angle H = LST - RA:
//   east = -cos(dec) sin(H),  north = sin(dec) cos(lat) - cos(dec) cos(H) sin(lat),
//   up   =  sin(dec) sin(lat) + cos(dec) cos(H) cos(lat)            (Meeus eq. 13.5/13.6).
// Stars below the horizon are not drawn. Rebuilt every 5 s (the sky turns 0.02 deg in that).
// ═══════════════════════════════════════════════════════════════════════════════════════════
static std::vector<LLVector4> sWolfStarCatalogue;   // ra_deg, dec_deg, vmag, bv
static bool sWolfStarCatalogueTried = false;

bool LLVOWLSky::loadStarCatalogue()
{
    if (sWolfStarCatalogueTried) return !sWolfStarCatalogue.empty();
    sWolfStarCatalogueTried = true;
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "wolf_stars.txt");
    std::ifstream in(path.c_str());
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream row(line);
        F32 ra, dec, vmag, bv;
        if (row >> ra >> dec >> vmag >> bv) sWolfStarCatalogue.emplace_back(ra, dec, vmag, bv);
    }
    LL_INFOS("WolfStars") << "star catalogue: " << sWolfStarCatalogue.size() << " stars from " << path << LL_ENDL;
    return !sWolfStarCatalogue.empty();
}

bool LLVOWLSky::updateRealStars(F64 lat_deg, F64 lon_deg)
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const F64 preview = WolfRegionWeather::instance().autoEnvTimeOffset();
    if (mRealStarsVerts.notNull() && now < mRealStarsNext && lat_deg == mRealStarsLat && lon_deg == mRealStarsLon
        && preview == mRealStarsPreview)
        return true;
    mRealStarsPreview = preview;
    mRealStarsNext = now + 5.0;
    mRealStarsLat = lat_deg;
    mRealStarsLon = lon_deg;

    const U32 count = (U32)sWolfStarCatalogue.size();
    if (mRealStarsVerts.isNull())
    {
        mRealStarsVerts = new LLVertexBuffer(LLDrawPoolWLSky::STAR_VERTEX_DATA_MASK);
        if (!mRealStarsVerts->allocateBuffer(count * 6, 0))
        {
            LL_WARNS("WolfStars") << "could not allocate " << count * 6 << " star vertices" << LL_ENDL;
            mRealStarsVerts = nullptr;
            return false;
        }
    }
    LLStrider<LLVector3> verticesp;
    LLStrider<LLColor4U> colorsp;
    LLStrider<LLVector2> texcoordsp;
    if (!mRealStarsVerts->getVertexStrider(verticesp) || !mRealStarsVerts->getColorStrider(colorsp)
        || !mRealStarsVerts->getTexCoord0Strider(texcoordsp))
    {
        return false;
    }

    // Julian day of the moment the sky shows — LLEnvironment's own clock plus any Preview time
    // (WolfRegionWeather::autoEnvSkyTime) — then GMST.
    const F64 jd = WolfRegionWeather::instance().autoEnvSkyTime() / 86400.0 + 2440587.5;
    const F64 t = (jd - 2451545.0) / 36525.0;
    F64 gmst = 280.46061837 + 360.98564736629 * (jd - 2451545.0) + 0.000387933 * t * t - t * t * t / 38710000.0;
    const F64 lst = fmod(fmod(gmst + lon_deg, 360.0) + 360.0, 360.0) * DEG_TO_RAD;
    const F64 slat = sin(lat_deg * DEG_TO_RAD), clat = cos(lat_deg * DEG_TO_RAD);
    const F32 radius = LLEnvironment::instance().getCurrentSky()->getDomeRadius();

    U32 drawn = 0;
    for (const LLVector4& st : sWolfStarCatalogue)
    {
        const F64 dec = st.mV[1] * DEG_TO_RAD;
        const F64 h = lst - st.mV[0] * DEG_TO_RAD;
        const F64 cd = cos(dec), sd = sin(dec);
        LLVector3 at((F32)(-cd * sin(h)), (F32)(sd * clat - cd * cos(h) * slat), (F32)(sd * slat + cd * cos(h) * clat));
        if (at.mV[VZ] <= 0.f) continue;                 // below the horizon
        const LLVector3 pos = at * radius;

        // Brightness: flux relative to a 6th-magnitude star, square-rooted so the faint ones
        // still show; 1st magnitude and brighter are full.
        const F32 flux = (F32)pow(10.0, -0.4 * (st.mV[2] - 6.0));
        const F32 intensity = llclamp(sqrtf(flux) / 10.f, 0.15f, 1.f);
        // Colour from B-V: blue-white hot stars to orange-red cool ones.
        const F32 bv = llclamp(st.mV[3], -0.4f, 2.0f);
        LLColor4 col;
        if (bv < 0.4f) col.set(0.70f + 0.75f * (bv + 0.4f) * 0.5f, 0.80f + 0.4f * (bv + 0.4f) * 0.5f, 1.f, intensity);
        else           col.set(1.f, llclamp(0.97f - 0.17f * (bv - 0.4f), 0.6f, 1.f), llclamp(0.92f - 0.30f * (bv - 0.4f), 0.4f, 1.f), intensity);
        col.clamp();

        LLVector3 left = at % LLVector3(0, 0, 1);
        if (left.lengthSquared() < 1e-6f) left.set(1.f, 0.f, 0.f);
        left.normVec();
        LLVector3 up = at % left;
        const F32 sc = 10.f + 22.f * intensity;
        left *= sc;
        up *= sc;
        *(verticesp++) = pos;
        *(verticesp++) = pos + up;
        *(verticesp++) = pos + left + up;
        *(verticesp++) = pos;
        *(verticesp++) = pos + left + up;
        *(verticesp++) = pos + left;
        *(texcoordsp++) = LLVector2(1, 0);
        *(texcoordsp++) = LLVector2(1, 1);
        *(texcoordsp++) = LLVector2(0, 1);
        *(texcoordsp++) = LLVector2(1, 0);
        *(texcoordsp++) = LLVector2(0, 1);
        *(texcoordsp++) = LLVector2(0, 0);
        const LLColor4U c4(col);
        for (int k = 0; k < 6; ++k) *(colorsp++) = c4;
        ++drawn;
    }
    mRealStarsVerts->unmapBuffer();
    mRealStarsDrawn = drawn;
    if (gSavedSettings.getBOOL("WolfShowAstronomy")) updateConstellations(lat_deg, lon_deg, jd);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// [ASTRONOMY 2026-10-03] World > Show Astronomy (Paul: "show and label the star groups like
// orion"). Figures: d3-celestial (github.com/ofrohn/d3-celestial, BSD 3-Clause, (c) 2015 Olaf
// Frohn) data/constellations.lines.json + constellations.json, converted by
// region-terrain-work/autoenv-stars/make_constellations.py to app_settings/wolf_constellations.txt
// (RA/Dec J2000 degrees). Placed with the same transform as the stars. Lines are thin quads in
// the star pass, flagged for starsF.glsl by texcoords (2,2); names are hover texts on the dome.
// ═══════════════════════════════════════════════════════════════════════════════════════════
struct WolfConstName { F32 ra; F32 dec; std::string name; };
static std::vector<LLVector4> sWolfConstSegs;      // ra1, dec1, ra2, dec2
static std::vector<WolfConstName> sWolfConstNames;
static bool sWolfConstTried = false;

bool LLVOWLSky::loadConstellations()
{
    if (sWolfConstTried) return !sWolfConstSegs.empty();
    sWolfConstTried = true;
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "wolf_constellations.txt");
    std::ifstream in(path.c_str());
    std::string line;
    while (std::getline(in, line))
    {
        if (line.size() < 2 || line[0] == '#') continue;
        std::istringstream row(line.substr(2));
        if (line[0] == 'L')
        {
            F32 a, b, c, d;
            if (row >> a >> b >> c >> d) sWolfConstSegs.emplace_back(a, b, c, d);
        }
        else if (line[0] == 'N')
        {
            WolfConstName n;
            if (row >> n.ra >> n.dec)
            {
                std::getline(row >> std::ws, n.name);
                sWolfConstNames.push_back(n);
            }
        }
    }
    LL_INFOS("WolfStars") << "constellations: " << sWolfConstSegs.size() << " segments, "
                          << sWolfConstNames.size() << " names from " << path << LL_ENDL;
    return !sWolfConstSegs.empty();
}

/** Horizon direction (+x east, +y north, +z up) of an RA/Dec, as updateRealStars places a star. */
static LLVector3 wolf_eq_to_enu(F64 ra_deg, F64 dec_deg, F64 lst_rad, F64 slat, F64 clat)
{
    const F64 dec = dec_deg * DEG_TO_RAD, h = lst_rad - ra_deg * DEG_TO_RAD;
    const F64 cd = cos(dec), sd = sin(dec);
    return LLVector3((F32)(-cd * sin(h)), (F32)(sd * clat - cd * cos(h) * slat), (F32)(sd * slat + cd * cos(h) * clat));
}

void LLVOWLSky::updateConstellations(F64 lat_deg, F64 lon_deg, F64 jd)
{
    if (!loadConstellations()) return;
    const F64 t = (jd - 2451545.0) / 36525.0;
    const F64 gmst = 280.46061837 + 360.98564736629 * (jd - 2451545.0) + 0.000387933 * t * t - t * t * t / 38710000.0;
    const F64 lst = fmod(fmod(gmst + lon_deg, 360.0) + 360.0, 360.0) * DEG_TO_RAD;
    const F64 slat = sin(lat_deg * DEG_TO_RAD), clat = cos(lat_deg * DEG_TO_RAD);
    // A hair inside the stars, so a figure's line never hides the star it joins.
    const F32 radius = LLEnvironment::instance().getCurrentSky()->getDomeRadius() * 0.995f;

    const U32 count = (U32)sWolfConstSegs.size();
    if (mConstVerts.isNull())
    {
        mConstVerts = new LLVertexBuffer(LLDrawPoolWLSky::STAR_VERTEX_DATA_MASK);
        if (!mConstVerts->allocateBuffer(count * 6, 0)) { mConstVerts = nullptr; return; }
    }
    LLStrider<LLVector3> v;
    LLStrider<LLColor4U> c;
    LLStrider<LLVector2> tc;
    if (!mConstVerts->getVertexStrider(v) || !mConstVerts->getColorStrider(c) || !mConstVerts->getTexCoord0Strider(tc)) return;
    // Soft sky blue, half transparent: a guide over the stars, not a stage set.
    // [10-03 Paul: "i turned on show astronomy it didn't appear"] was 110 alpha and 0.0006 —
    // about one faint pixel, and further dimmed by the night fade: invisible in practice.
    const LLColor4U line_colour(255, 255, 255, 235);   // bright white (Paul 10-03)
    const F32 half_width = radius * 0.0012f;     // ~0.14 degrees: about two to three pixels at 1080p
    U32 drawn = 0;
    for (const LLVector4& sg : sWolfConstSegs)
    {
        const LLVector3 a = wolf_eq_to_enu(sg.mV[0], sg.mV[1], lst, slat, clat);
        const LLVector3 b = wolf_eq_to_enu(sg.mV[2], sg.mV[3], lst, slat, clat);
        if (a.mV[VZ] <= 0.f && b.mV[VZ] <= 0.f) continue;     // wholly below the horizon
        const LLVector3 pa = a * radius, pb = b * radius;
        LLVector3 side = (pb - pa) % (a + b);
        if (side.lengthSquared() < 1e-6f) continue;
        side.normalize();
        side *= half_width;
        *(v++) = pa - side; *(v++) = pb - side; *(v++) = pb + side;
        *(v++) = pa - side; *(v++) = pb + side; *(v++) = pa + side;
        for (int k = 0; k < 6; ++k) { *(tc++) = LLVector2(2.f, 2.f); *(c++) = line_colour; }
        ++drawn;
    }
    mConstVerts->unmapBuffer();
    mConstDrawn = drawn;

    // Names, on the dome where the figure is. Hover texts are placed in agent space round the
    // camera, so they are moved with it every frame in drawStars; here only their directions change.
    if (mConstNames.size() != sWolfConstNames.size())
    {
        hideConstellationNames();
        mConstNames.clear();
        for (size_t i = 0; i < sWolfConstNames.size(); ++i)
        {
            LLHUDText* txt = (LLHUDText*)LLHUDObject::addHUDObject(LLHUDObject::LL_HUD_TEXT);
            // Not depth-compared: the label sits on the far sky dome, and in the deferred pipeline
            // the UI-pass depth test hid it entirely. Names below the horizon are hidden instead.
            txt->setZCompare(false);
            txt->setDoFade(false);
            txt->setFont(LLFontGL::getFontSansSerif());
            txt->setColor(LLColor4(1.f, 1.f, 1.f, 1.f));
            txt->setString(sWolfConstNames[i].name);
            txt->setHidden(true);
            mConstNames.push_back(txt);
        }
    }
    for (size_t i = 0; i < mConstNames.size(); ++i)
    {
        const LLVector3 d = wolf_eq_to_enu(sWolfConstNames[i].ra, sWolfConstNames[i].dec, lst, slat, clat);
        if (mConstDirs.size() != mConstNames.size()) mConstDirs.resize(mConstNames.size());
        mConstDirs[i] = d;                        // moved round the camera every frame in drawStars
    }
}

LLVOWLSky::~LLVOWLSky()
{
    for (auto& t : mConstNames) if (t.notNull()) t->markDead();
    mConstNames.clear();
}

bool LLVOWLSky::drawConstellations()
{
    // [ASTRONOMY 2026-10-03] Only while the real star map is drawn and Show Astronomy is on.
    static LLCachedControl<bool> show_astronomy(gSavedSettings, "WolfShowAstronomy", false);
    if (!mRealStarsActive || !show_astronomy || mConstVerts.isNull() || mConstDrawn == 0) return false;
    mConstVerts->setBuffer();
    mConstVerts->drawArrays(LLRender::TRIANGLES, 0, mConstDrawn * 6);
    return true;
}

void LLVOWLSky::hideConstellationNames()
{
    for (auto& t : mConstNames) if (t.notNull()) t->setHidden(true);
}

void LLVOWLSky::drawStars(void)
{
    // <WolfViewer 2026-10-03> The real sky on automatic regions.
    mRealStarsActive = false;
    if (WolfRegionWeather::instance().autoEnvRealSky() && loadStarCatalogue()
        && updateRealStars(WolfRegionWeather::instance().autoEnvLat(), WolfRegionWeather::instance().autoEnvLon()))
    {
        mRealStarsActive = true;
        if (mRealStarsDrawn > 0)
        {
            mRealStarsVerts->setBuffer();
            mRealStarsVerts->drawArrays(LLRender::TRIANGLES, 0, mRealStarsDrawn * 6);
        }
        // [ASTRONOMY 2026-10-03] World > Show Astronomy.
        static LLCachedControl<bool> show_astronomy(gSavedSettings, "WolfShowAstronomy", false);
        if (show_astronomy && mConstVerts.notNull() && mConstDrawn > 0)
        {
            // The lines are drawn in their own pass AFTER the clouds (drawConstellations, from
            // LLDrawPoolWLSky::renderConstellationsDeferred). Paul 10-03: "as the stars show show
            // astronomy" — the names are full bright white whenever the star pass draws at all
            // (this code only runs then: renderStarsDeferred returns early below 0.001).
            const F32 fade = 1.f;
            const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
            for (size_t i = 0; i < mConstNames.size() && i < sWolfConstNames.size(); ++i)
            {
                LLHUDText* txt = mConstNames[i];
                if (!txt) continue;
                const LLVector3 dir = mConstDirs.size() > i ? mConstDirs[i] : LLVector3::zero;
                const bool up = dir.mV[VZ] > 0.03f && fade > 0.02f;
                txt->setHidden(!up);
                if (!up) continue;
                // Inside the far clip whatever the draw distance, so the hover text is drawn and
                // still depth-tested against the scene in front of it.
                const F32 dist = llclamp(LLViewerCamera::getInstance()->getFar() * 0.8f, 50.f, 900.f);
                txt->setPositionAgent(cam + dir * dist);
                txt->setAlpha(fade);
            }
        }
        else
        {
            hideConstellationNames();
        }
        return;
    }
    hideConstellationNames();
    //  render the stars as a sphere centered at viewer camera
    if (mStarsVerts.notNull())
    {
        mStarsVerts->setBuffer();
        mStarsVerts->drawArrays(LLRender::TRIANGLES, 0, getStarsNumVerts()*4);
    }
}

void LLVOWLSky::drawFsSky(void)
{
    if (mFsSkyVerts.isNull())
    {
        updateGeometry(mDrawable);
    }

    LLGLDisable disable_blend(GL_BLEND);

    mFsSkyVerts->setBuffer();
    mFsSkyVerts->drawRange(LLRender::TRIANGLES, 0, mFsSkyVerts->getNumVerts() - 1, mFsSkyVerts->getNumIndices(), 0);
    gPipeline.addTrianglesDrawn(mFsSkyVerts->getNumIndices());
    LLVertexBuffer::unbind();
}

void LLVOWLSky::drawDome(void)
{
    if (mStripsVerts.empty())
    {
        updateGeometry(mDrawable);
    }

    LLGLDepthTest gls_depth(GL_TRUE, GL_FALSE);

    std::vector< LLPointer<LLVertexBuffer> >::const_iterator strips_vbo_iter, end_strips;
    end_strips = mStripsVerts.end();
    for(strips_vbo_iter = mStripsVerts.begin(); strips_vbo_iter != end_strips; ++strips_vbo_iter)
    {
        LLVertexBuffer * strips_segment = strips_vbo_iter->get();

        strips_segment->setBuffer();

        strips_segment->drawRange(
            LLRender::TRIANGLE_STRIP,
            0, strips_segment->getNumVerts()-1, strips_segment->getNumIndices(),
            0);
        gPipeline.addTrianglesDrawn(strips_segment->getNumIndices());
    }

    LLVertexBuffer::unbind();
}

void LLVOWLSky::initStars()
{
    const F32 DISTANCE_TO_STARS = LLEnvironment::instance().getCurrentSky()->getDomeRadius();

    // Initialize star map
    mStarVertices.resize(getStarsNumVerts());
    mStarColors.resize(getStarsNumVerts());
    mStarIntensities.resize(getStarsNumVerts());

    std::vector<LLVector3>::iterator v_p = mStarVertices.begin();
    std::vector<LLColor4>::iterator v_c = mStarColors.begin();
    std::vector<F32>::iterator v_i = mStarIntensities.begin();

    U32 i;

    for (i = 0; i < getStarsNumVerts(); ++i)
    {
        v_p->mV[VX] = ll_frand() - 0.5f;
        v_p->mV[VY] = ll_frand() - 0.5f;

        // we only want stars on the top half of the dome!

        v_p->mV[VZ] = ll_frand()/2.f;

        v_p->normVec();
        *v_p *= DISTANCE_TO_STARS;
        *v_i = llmin((F32)pow(ll_frand(),2.f) + 0.1f, 1.f);
        v_c->mV[VRED]   = 0.75f + ll_frand() * 0.25f ;
        v_c->mV[VGREEN] = 1.f ;
        v_c->mV[VBLUE]  = 0.75f + ll_frand() * 0.25f ;
        v_c->mV[VALPHA] = 1.f;
        v_c->clamp();
        v_p++;
        v_c++;
        v_i++;
    }
}

void LLVOWLSky::buildStripsBuffer(U32 begin_stack,
                                  U32 end_stack,
                                  LLStrider<LLVector3> & vertices,
                                  LLStrider<LLVector2> & texCoords,
                                  LLStrider<U16> & indices,
                                  const F32 dome_radius,
                                  const U32& num_slices,
                                  const U32& num_stacks)
{
    U32 i, j;
    F32 phi0, theta, x0, y0, z0;
    const F32 reciprocal_num_stacks = 1.f / num_stacks;

    llassert(end_stack <= num_stacks);

    // stacks are iterated one-indexed since phi(0) was handled by the fan above
#if NEW_TESS
    for(i = begin_stack; i <= end_stack; ++i)
#else
    for(i = begin_stack + 1; i <= end_stack+1; ++i)
#endif
    {
        phi0 = calcPhi(i, reciprocal_num_stacks);

        for(j = 0; j < num_slices; ++j)
        {
            theta = F_TWO_PI * (float(j) / float(num_slices));

            // standard transformation from  spherical to
            // rectangular coordinates
            x0 = sin(phi0) * cos(theta);
            y0 = cos(phi0);
            z0 = sin(phi0) * sin(theta);

#if NEW_TESS
            *vertices++ = LLVector3(x0 * dome_radius, y0 * dome_radius, z0 * dome_radius);
#else
            if (i == num_stacks-2)
            {
                *vertices++ = LLVector3(x0*dome_radius, y0*dome_radius-1024.f*2.f, z0*dome_radius);
            }
            else if (i == num_stacks-1)
            {
                *vertices++ = LLVector3(0, y0*dome_radius-1024.f*2.f, 0);
            }
            else
            {
                *vertices++     = LLVector3(x0 * dome_radius, y0 * dome_radius, z0 * dome_radius);
            }
#endif

            // generate planar uv coordinates
            // note: x and z are transposed in order for things to animate
            // correctly in the global coordinate system where +x is east and
            // +y is north
            *texCoords++    = LLVector2((-z0 + 1.f) / 2.f, (-x0 + 1.f) / 2.f);
        }
    }

    //build triangle strip...
    *indices++ = 0 ;

    S32 k = 0 ;
    for(i = 1; i <= end_stack - begin_stack; ++i)
    {
        *indices++ = i * num_slices + k ;

        k = (k+1) % num_slices ;
        for(j = 0; j < num_slices ; ++j)
        {
            *indices++ = (i-1) * num_slices + k ;
            *indices++ = i * num_slices + k ;

            k = (k+1) % num_slices ;
        }

        if((--k) < 0)
        {
            k = num_slices - 1 ;
        }

        *indices++ = i * num_slices + k ;
    }
}

void LLVOWLSky::updateStarColors()
{
    std::vector<LLColor4>::iterator v_c = mStarColors.begin();
    std::vector<F32>::iterator v_i = mStarIntensities.begin();
    std::vector<LLVector3>::iterator v_p = mStarVertices.begin();

    const F32 var = 0.15f;
    const F32 min = 0.5f; //0.75f;
    //const F32 sunclose_max = 0.6f;
    //const F32 sunclose_range = 1 - sunclose_max;

    //F32 below_horizon = - llmin(0.0f, gSky.mVOSkyp->getToSunLast().mV[2]);
    //F32 brightness_factor = llmin(1.0f, below_horizon * 20);

    static S32 swap = 0;
    swap++;

    if ((swap % 2) == 1)
    {
        F32 intensity;                      //  max intensity of each star
        U32 x;
        for (x = 0; x < getStarsNumVerts(); ++x)
        {
            //F32 sundir_factor = 1;
            LLVector3 tostar = *v_p;
            tostar.normVec();
            //const F32 how_close_to_sun = tostar * gSky.mVOSkyp->getToSunLast();
            //if (how_close_to_sun > sunclose_max)
            //{
            //  sundir_factor = (1 - how_close_to_sun) / sunclose_range;
            //}
            intensity = *(v_i);
            F32 alpha = v_c->mV[VALPHA] + (ll_frand() - 0.5f) * var * intensity;
            if (alpha < min * intensity)
            {
                alpha = min * intensity;
            }
            if (alpha > intensity)
            {
                alpha = intensity;
            }
            //alpha *= brightness_factor * sundir_factor;

            alpha = llclamp(alpha, 0.f, 1.f);
            v_c->mV[VALPHA] = alpha;
            v_c++;
            v_i++;
            v_p++;
        }
    }
}

bool LLVOWLSky::updateStarGeometry(LLDrawable *drawable)
{
    LLStrider<LLVector3> verticesp;
    LLStrider<LLColor4U> colorsp;
    LLStrider<LLVector2> texcoordsp;

    if (mStarsVerts.isNull())
    {
        mStarsVerts = new LLVertexBuffer(LLDrawPoolWLSky::STAR_VERTEX_DATA_MASK);
        if (!mStarsVerts->allocateBuffer(getStarsNumVerts()*6, 0))
        {
            LL_WARNS() << "Failed to allocate Vertex Buffer for Sky to " << getStarsNumVerts() * 6 << " vertices" << LL_ENDL;
        }
    }

    bool success = mStarsVerts->getVertexStrider(verticesp)
        && mStarsVerts->getColorStrider(colorsp)
        && mStarsVerts->getTexCoord0Strider(texcoordsp);

    if(!success)
    {
        LL_ERRS() << "Failed updating star geometry." << LL_ENDL;
    }

    // *TODO: fix LLStrider with a real prefix increment operator so it can be
    // used as a model of OutputIterator. -Brad
    // std::copy(mStarVertices.begin(), mStarVertices.end(), verticesp);

    if (mStarVertices.size() < getStarsNumVerts())
    {
        LL_ERRS() << "Star reference geometry insufficient." << LL_ENDL;
    }

    for (U32 vtx = 0; vtx < getStarsNumVerts(); ++vtx)
    {
        LLVector3 at = mStarVertices[vtx];
        at.normVec();
        LLVector3 left = at%LLVector3(0,0,1);
        LLVector3 up = at%left;

        F32 sc = 16.0f + (ll_frand() * 20.0f);
        left *= sc;
        up *= sc;

        *(verticesp++)  = mStarVertices[vtx];
        *(verticesp++) = mStarVertices[vtx]+up;
        *(verticesp++) = mStarVertices[vtx]+left+up;
        *(verticesp++)  = mStarVertices[vtx];
        *(verticesp++) = mStarVertices[vtx]+left+up;
        *(verticesp++) = mStarVertices[vtx]+left;

        *(texcoordsp++) = LLVector2(1,0);
        *(texcoordsp++) = LLVector2(1,1);
        *(texcoordsp++) = LLVector2(0,1);
        *(texcoordsp++) = LLVector2(1,0);
        *(texcoordsp++) = LLVector2(0,1);
        *(texcoordsp++) = LLVector2(0,0);

        // <FS:ND> Only convert to LLColour4U once

        // *(colorsp++)    = LLColor4U(mStarColors[vtx]);
        // *(colorsp++)    = LLColor4U(mStarColors[vtx]);
        // *(colorsp++)    = LLColor4U(mStarColors[vtx]);
        // *(colorsp++)    = LLColor4U(mStarColors[vtx]);
        // *(colorsp++)    = LLColor4U(mStarColors[vtx]);
        // *(colorsp++)    = LLColor4U(mStarColors[vtx]);

        LLColor4U color4u(mStarColors[vtx]);
        *(colorsp++)    = color4u;
        *(colorsp++)    = color4u;
        *(colorsp++)    = color4u;
        *(colorsp++)    = color4u;
        *(colorsp++)    = color4u;
        *(colorsp++)    = color4u;

        // </FS:ND>

    }

    mStarsVerts->unmapBuffer();
    return true;
}
