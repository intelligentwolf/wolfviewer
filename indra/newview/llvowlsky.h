/**
 * @file llvowlsky.h
 * @brief LLVOWLSky class definition
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

#ifndef LL_VOWLSKY_H
#define LL_VOWLSKY_H

#include "llviewerobject.h"

class LLVOWLSky : public LLStaticViewerObject {
private:
    inline static U32 getNumStacks(void);
    inline static U32 getNumSlices(void);
    inline static U32 getStripsNumVerts(void);
    inline static U32 getStripsNumIndices(void);
    inline static U32 getStarsNumVerts(void);
    inline static U32 getStarsNumIndices(void);

public:
    LLVOWLSky(const LLUUID &id, const LLPCode pcode, LLViewerRegion *regionp);
    ~LLVOWLSky();   // <WolfViewer 2026-10-03/> releases the constellation names

    /*virtual*/ void         idleUpdate(LLAgent &agent, const F64 &time);
    /*virtual*/ bool         isActive(void) const;
    /*virtual*/ LLDrawable * createDrawable(LLPipeline *pipeline);
    /*virtual*/ bool         updateGeometry(LLDrawable *drawable);

    void drawStars(void);
    /** <WolfViewer 2026-10-03> True while the real star map is drawn (Jimmy Olsen's Automatic
     *  Environment): the draw pool then leaves out the stock star field's slow spin. */
    bool drawingRealStars() const { return mRealStarsActive; }
    /** [ASTRONOMY 2026-10-03] World > Show Astronomy lines; drawn after the clouds. */
    bool drawConstellations();
    /** [ASTRONOMY 2026-10-03] Hide the names (the star pass is skipped in daylight, so it cannot). */
    void hideConstellationNames();
    void drawDome(void);
    void drawFsSky(void); // fullscreen sky for advanced atmo
    void resetVertexBuffers(void);

    void cleanupGL();
    void restoreGL();

private:

    // helper function for initializing the stars.
    void initStars();

    // helper function for building the strips vertex buffer.
    // note begin_stack and end_stack follow stl iterator conventions,
    // begin_stack is the first stack to be included, end_stack is the first
    // stack not to be included.
    static void buildStripsBuffer(U32 begin_stack, U32 end_stack,
                                  LLStrider<LLVector3> & vertices,
                                  LLStrider<LLVector2> & texCoords,
                                  LLStrider<U16> & indices,
                                  const F32 RADIUS,
                                  const U32& num_slices,
                                  const U32& num_stacks);

    // helper function for updating the stars colors.
    void updateStarColors();

    // helper function for updating the stars geometry.
    bool updateStarGeometry(LLDrawable *drawable);

private:
    LLPointer<LLVertexBuffer>                   mFsSkyVerts;
    std::vector< LLPointer<LLVertexBuffer> >    mStripsVerts;
    LLPointer<LLVertexBuffer>                   mStarsVerts;

    std::vector<LLVector3>  mStarVertices;              // Star verticies
    std::vector<LLColor4>   mStarColors;                // Star colors
    std::vector<F32>        mStarIntensities;           // Star intensities

    // <WolfViewer 2026-10-03> The real star map — Yale Bright Star Catalogue (app_settings/
    // wolf_stars.txt), placed for the region's real place and the real time.
    struct WolfStar { F32 ra; F32 dec; F32 vmag; F32 bv; };
    static bool loadStarCatalogue();
    bool updateRealStars(F64 lat_deg, F64 lon_deg);
    LLPointer<LLVertexBuffer> mRealStarsVerts;
    U32  mRealStarsDrawn = 0;
    F64  mRealStarsNext = 0.0;
    F64  mRealStarsLat = 999.0;
    F64  mRealStarsLon = 999.0;
    F64  mRealStarsPreview = 0.0;
    // [ASTRONOMY 2026-10-03] World > Show Astronomy: constellation figures + names.
    static bool loadConstellations();
    void updateConstellations(F64 lat_deg, F64 lon_deg, F64 jd);
    LLPointer<LLVertexBuffer> mConstVerts;
    U32  mConstDrawn = 0;
    std::vector<LLPointer<class LLHUDText>> mConstNames;
    std::vector<LLVector3> mConstDirs;
    bool mRealStarsActive = false;
};

#endif // LL_VOWLSKY_H
