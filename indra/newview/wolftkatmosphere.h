/**
 * @file wolftkatmosphere.h
 * @brief WolfViewer: takram three-geospatial's precomputed atmosphere (Bruneton's model), translated.
 *
 * Translated for WolfViewer (IntelligentWolf Ltd, 2026-10-12) from
 *   three-geospatial (github.com/takram-design-engineering/three-geospatial, commit
 *   b012ad06d858fc035d88aacfd73f092f93c994e4, MIT, (c) 2024 Shota Matsuda):
 *   packages/atmosphere/src/{PrecomputedTexturesGenerator.ts, AtmosphereParameters.ts, constants.ts,
 *   AtmosphereMaterialBase.ts, getSunLightColor.ts, SkyLightProbe.ts, helpers/sampleTexture.ts,
 *   helpers/functions.ts, shaders/bruneton/runtime.glsl}
 *   and Eric Bruneton's precomputed_atmospheric_scattering (github.com/ebruneton/precomputed_atmospheric_scattering,
 *   commit 34f14e745cff948f4ca3157d1b62a445ffa7286f, BSD-3, (c) 2017 Eric Bruneton):
 *   atmosphere/{model.cc, model.h, constants.h}, atmosphere/demo/demo.cc.
 * The licence texts: indra/newview/THIRD_PARTY_SKY.txt.
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

#ifndef WOLF_TK_ATMOSPHERE_H
#define WOLF_TK_ATMOSPHERE_H

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include "llglslshader.h"
#include "v3color.h"
#include "v3math.h"

struct WolfEnvDoc;

namespace WolfTk
{
    // Source: three-geospatial packages/atmosphere/src/constants.ts:8-19 (the table sizes)
    constexpr S32 IRRADIANCE_TEXTURE_WIDTH = 64;
    constexpr S32 IRRADIANCE_TEXTURE_HEIGHT = 16;
    constexpr S32 SCATTERING_TEXTURE_R_SIZE = 32;
    constexpr S32 SCATTERING_TEXTURE_MU_SIZE = 128;
    constexpr S32 SCATTERING_TEXTURE_MU_S_SIZE = 32;
    constexpr S32 SCATTERING_TEXTURE_NU_SIZE = 8;
    constexpr S32 SCATTERING_TEXTURE_WIDTH = SCATTERING_TEXTURE_NU_SIZE * SCATTERING_TEXTURE_MU_S_SIZE;
    constexpr S32 SCATTERING_TEXTURE_HEIGHT = SCATTERING_TEXTURE_MU_SIZE;
    constexpr S32 SCATTERING_TEXTURE_DEPTH = SCATTERING_TEXTURE_R_SIZE;
    constexpr S32 TRANSMITTANCE_TEXTURE_WIDTH = 256;
    constexpr S32 TRANSMITTANCE_TEXTURE_HEIGHT = 64;
    // Source: constants.ts:21 METER_TO_LENGTH_UNIT = 1 / 1000 (the shaders' length unit is the kilometre)
    constexpr F64 METER_TO_LENGTH_UNIT = 1.0 / 1000.0;

    /** Source: ebruneton atmosphere/model.h DensityProfileLayer (SI: width m, exp_scale and linear_term 1/m);
     *  three-geospatial AtmosphereParameters.ts:52-86 is the same layer in km. */
    struct DensityProfileLayer
    {
        F64 width = 0.0, exp_term = 0.0, exp_scale = 0.0, linear_term = 0.0, constant_term = 0.0;
    };

    /** The inputs of Bruneton's Model constructor (ebruneton atmosphere/model.cc:613-632), in SI units, per
     *  wavelength (nm). */
    struct AtmosphereSpectrum
    {
        std::vector<F64> wavelengths, solar_irradiance, rayleigh_scattering, mie_scattering, mie_extinction,
                         absorption_extinction, ground_albedo;
        F64 sun_angular_radius = 0.0, bottom_radius = 0.0, top_radius = 0.0, mie_phase_function_g = 0.0,
            max_sun_zenith_angle = 0.0;
        DensityProfileLayer rayleigh_density[2], mie_density[2], absorption_density[2];

        /** Earth: ebruneton atmosphere/demo/demo.cc:188-284 Demo::InitModel. */
        static AtmosphereSpectrum earth();
        /** A Wolf Environment document's atmosphere (wolfenvironmentstate.h WolfEnvDoc) on Earth's spectrum. */
        static AtmosphereSpectrum fromDoc(const WolfEnvDoc& d);
    };

    /** Changes whenever anything the precompute depends on changes. */
    size_t hashOf(const AtmosphereSpectrum& s);

    /**
     * The precomputed tables (transmittance, scattering, irradiance, higher-order scattering) of
     * PrecomputedTexturesGenerator.ts, computed as Bruneton's Model::Init (model.cc:866-975) drives the
     * precompute: 3 wavelengths (680 / 550 / 440 nm, the takram default) or, with more, precomputed
     * illuminance (model.cc:914-964) - WolfViewer uses 15 (demo.cc:280-282 use_luminance == PRECOMPUTED).
     *
     * Two sets of final tables: a build writes the other set over several frames (precomputeStep) and they
     * swap only when it is complete, so the sky keeps drawing with the previous atmosphere meanwhile.
     */
    class Atmosphere
    {
    public:
        /** Every define the runtime shaders need (AtmosphereMaterialBase.ts:147-158, :240-263;
         *  AerialPerspectiveEffect.ts:291-301, :549-571). */
        static void addDefines(LLGLSLShader& sh);

        bool loadShaders(S32 shader_level);
        void unloadShaders();

        /** Starts a build for `spectrum` with `num_wavelengths` precomputed wavelengths (3 or 15); replaces a
         *  build in progress. Returns false (nothing started) while the programs or the pipeline's screen
         *  triangle are not there; the caller asks again later. */
        bool beginPrecompute(const AtmosphereSpectrum& spectrum, U32 num_wavelengths);
        /** One step of the build (a yield point of PrecomputedTexturesGenerator.ts:111-128 iterateIdle, finer:
         *  at most 8 layers of a 3D pass). Returns true when the build has finished (or none is running). */
        bool precomputeStep();
        bool ready() const { return mComplete >= 0; }
        bool building() const { return mBuilding; }

        /** On the BOUND program: ATMOSPHERE (at 680 / 550 / 440 nm, km), SUN_ / SKY_SPECTRAL_RADIANCE_TO_LUMINANCE
         *  (Bruneton's absolute k x pre_exposure) and the four tables (complete set). */
        void setRuntimeUniforms(LLGLSLShader& sh, F32 pre_exposure, bool uniforms = true) const;   // uniforms false: the tables only
        /** [WolfViewer 2026-10-13] The region's EEP (wolfenvironmentrender.cpp updateLight): the clouds' light tint
         *  (cloudsF.glsl wolfEepCloudTint, sent as tint - 1; white = none) and haze_horizon / 0.19 (wolfEepMieScale, sent as
         *  scale - 1). */
        void setEepColours(const LLColor3& cloud, F32 mie_scale) { mEepCloud = cloud; mEepMie = mie_scale; }
        /** [WolfViewer 2026-10-13] The sky's gains toward the EEP (brunetonRuntime.glsl wolfEepGain[EEP_GAINS] / wolfEepGainOn;
         *  wolfenvironmentrender.cpp probeSky). on false: the physical sky as it is. */
        /** [WolfViewer 2026-10-26] 7 heights (90 40 20 10 5 2 deg and the true horizon) x 6 angles to the stock sky's light (3 10 25
         *  50 90 180 deg), each node probed on both sides of the light (brunetonRuntime.glsl WOLF_EEP_GAIN_ROWS / _COLS /
         *  WOLF_EEP_GAINS / WOLF_EEP_PROBES: the same numbers). */
        static constexpr S32 EEP_GAIN_ROWS = 7, EEP_GAIN_COLS = 6, EEP_GAINS = EEP_GAIN_ROWS * EEP_GAIN_COLS, EEP_PROBES = 2 * EEP_GAINS;
        void setEepGains(const LLColor3 gains[EEP_GAINS], bool on) { for (S32 i = 0; i < EEP_GAINS; ++i) mEepGain[i] = gains[i]; mEepGainOn = on; mEepBlockDirty = true; }
        /** [WolfViewer 2026-10-25] The directions (world) skyF.glsl's probe mode draws, pixel i toward dirs[i] (wolfEepProbeDir in the
         *  WolfEepGains block): written into the block and bound at once, for the probe draw that follows (GL thread). */
        void setEepProbeDirs(const LLVector3 dirs[EEP_PROBES]);
        /** [WolfViewer 2026-10-25] The directions (ECEF, this frame's) the sky's and the clouds' gain azimuths are measured from
         *  (brunetonRuntime.glsl wolfEepGainRef, cloudsF.glsl wolfEepCloudGainRef); set every frame (the ECEF frame moves). */
        void setEepGainRefs(const LLVector3& sky_ref_ecef, const LLVector3& cloud_ref_ecef) { mEepGainRef = sky_ref_ecef; mEepCloudGainRef = cloud_ref_ecef; }
        /** [WolfViewer 2026-10-31] For a program setRuntimeUniforms already set up this frame, after setEepGains / setEepGainRefs
         *  changed: the WolfEepGains block uploaded and bound, and wolfEepGainRef set (wolfenvironmentrender.cpp probeSky, the
         *  reading taken just before the dome's draw - without it that draw read the block and the reference as they were). */
        void refreshEepGains(LLGLSLShader& sh) const;
        /** [WolfViewer 2026-10-25] The night's light as the eye sees it (wolftk/nightLight.glsl wolfNight / wolfNightS): how far into
         *  night (WolfEnvironmentRender::nightFactor, 0 = off) and the rods' saturation s at the frame's adaptation luminance. */
        void setNightLight(F32 night, F32 s) { mNight = night; mNightS = s; }
        /** [WolfViewer 2026-10-13] The clouds' colour gains toward the EEP, the same grid (cloudsF.glsl wolfEepCloudGain[EEP_GAINS] /
         *  wolfEepCloudGainOn; wolfenvironmentrender.cpp measureCloudCover). */
        /** [WolfViewer round 16] the clouds' thinning (cloudsF.glsl wolfEepCloudThin): 0 = as marched, toward 1 more transparent */
        void setEepCloudThin(F32 thin) { mEepCloudThin = thin; }
        /** [WolfViewer 2026-10-27] the clouds take the sky's per-direction gain (cloudsF.glsl wolfEepCloudSkyGain), used while
         *  the cloud gains themselves are off: Wolf weather by day (wolfenvironmentrender.cpp measureCloudCover) */
        void setEepCloudSkyGain(bool on) { mEepCloudSkyGain = on; }
        void setEepCloudGains(const LLColor3 gains[EEP_GAINS], bool on) { for (S32 i = 0; i < EEP_GAINS; ++i) mEepCloudGain[i] = gains[i]; mEepCloudGainOn = on; mEepBlockDirty = true; }

        GLuint transmittanceTexture() const;
        GLuint scatteringTexture() const;
        GLuint irradianceTexture() const;
        GLuint higherOrderScatteringTexture() const;

        /** The sun's illuminance at a point (ECEF km) toward `sun_dir`, unexposed (getSunLightColor.ts:48-103 with
         *  Bruneton's absolute SUN k in place of the relative one). */
        LLColor3 sunIlluminance(const LLVector3& pos_km, const LLVector3& sun_dir) const;
        /** The sky's irradiance texture value at a point x SKY k, unexposed (SkyLightProbe.ts:85-129 before the
         *  SH coefficients). */
        LLColor3 skyIrradiance(const LLVector3& pos_km, const LLVector3& sun_dir) const;
        /** SkyLightProbe.ts:124-129: L0 and L1 SH coefficients (three.js order) for the up normal `normal_world`. */
        void skySH(const LLVector3& pos_km, const LLVector3& sun_dir, const LLVector3& normal_world, LLColor3 coeffs[4]) const;
        /** runtime.glsl:373-391 GetSunAndSkyIrradiance (x the k's, Bruneton's GetSunAndSkyIlluminance :436-444) on a
         *  horizontal surface (normal = the radial up): sun + sky, unexposed. */
        LLColor3 horizontalIlluminance(const LLVector3& pos_km, const LLVector3& sun_dir, LLColor3* sun_part, LLColor3* sky_part) const;

        F32 bottomRadiusKm() const { return (F32)mRuntime.bottom_radius; }
        F32 topRadiusKm() const { return (F32)mRuntime.top_radius; }
        F32 sunAngularRadius() const { return (F32)mRuntime.sun_angular_radius; }
        const LLVector3& sunK() const { return mSunK; }
        const LLVector3& skyK() const { return mSkyK; }
        /** [WOLF ENV TAKRAM 2026-10-12] The sun's illuminance above the atmosphere, unexposed: ATMOSPHERE.solar_irradiance x
         *  the absolute SUN k (getSunLightColor.ts:86-88: r > topRadius gives transmittance 1). */
        LLColor3 sunIlluminanceAboveAtmosphere() const
        {
            return LLColor3((F32)(mRuntime.solar_irradiance[0] * mSunK.mV[0]), (F32)(mRuntime.solar_irradiance[1] * mSunK.mV[1]),
                            (F32)(mRuntime.solar_irradiance[2] * mSunK.mV[2]));
        }
        bool programsReady() const { return mShadersLoaded; }
        /** The sun's colour above the atmosphere of the tables in use, mean 1 (Bruneton demo.cc:327-340 white balance). */
        LLColor3 whitePoint() const { return mWhitePoint; }
        /** A build could not start (its programs or the screen triangle are missing). */
        bool failed() const { return !mShadersLoaded; }

        void release();

        /** The atmosphere uniform values at three wavelengths, km (model.cc:636-743). */
        struct Params
        {
            F64 solar_irradiance[3] = {}, rayleigh_scattering[3] = {}, mie_scattering[3] = {}, mie_extinction[3] = {},
                absorption_extinction[3] = {}, ground_albedo[3] = {};
            F64 sun_angular_radius = 0.0, bottom_radius = 0.0, top_radius = 0.0, mie_phase_function_g = 0.0, mu_s_min = 0.0;
            DensityProfileLayer rayleigh_density[2], mie_density[2], absorption_density[2];
        };

    private:
        struct Set { GLuint transmittance = 0, scattering = 0, irradiance = 0, higher = 0; };
        void allocateSet(Set& s);
        void freeSet(Set& s);
        void allocateContext();
        void freeContext();
        void buildSteps();
        void readBack();
        void setParams(GLuint program, const Params& p) const;
        GLint location(GLuint program, const char* name) const;

        enum Pass { TRANS = 0, DIRECT_DELTA, DIRECT_IRR, SINGLE_RAY, SINGLE_MIE, SINGLE_SCAT, DENSITY,
                    INDIRECT_DELTA, INDIRECT_IRR, MULTI_DELTA, MULTI_SCAT, PASS_COUNT };
        LLGLSLShader mPrograms[PASS_COUNT];
        LLColor3 mEepCloud = LLColor3::white;
        LLColor3 mEepGain[EEP_GAINS];   // read by the shaders only while mEepGainOn (setEepGains fills them first)
        bool     mEepGainOn = false;
        LLColor3 mEepCloudGain[EEP_GAINS];   // read by the shaders only while mEepCloudGainOn
        bool     mEepCloudGainOn = false;
        bool     mEepCloudSkyGain = false;   // [2026-10-27] setEepCloudSkyGain
        LLVector3 mEepGainRef = LLVector3::z_axis, mEepCloudGainRef = LLVector3::z_axis;   // setEepGainRefs
        F32      mNight = 0.f, mNightS = 1.f;   // setNightLight
        // [WolfViewer 2026-10-25] the WolfEepGains uniform block (brunetonRuntime.glsl; LLGLSLShader::UB_WOLF_EEP_GAINS): the sky's
        // gains, the clouds' gains and the probe directions, std140 vec4 each; uploaded when the gains change (bindEepBlock)
        mutable GLuint mEepBlock = 0;
        mutable bool   mEepBlockDirty = true;
        void bindEepBlock() const;
        F32      mEepCloudThin = 0.f;
        F32      mEepMie = 1.f;   // the EEP's haze_horizon / 0.19 (brunetonRuntime.glsl wolfEepMieScale)
        bool mShadersLoaded = false;

        Set mSets[2];
        S32 mComplete = -1;   // the set the runtime uses
        S32 mTarget = 0;      // the set a build writes
        // PrecomputedTexturesGenerator.ts:152-208 Context
        GLuint mDeltaIrradiance = 0, mDeltaRayleigh = 0, mDeltaMie = 0, mDeltaDensity = 0;
        GLuint mFbo = 0;

        AtmosphereSpectrum mSpectrum;
        U32 mNumWavelengths = 3;
        bool mBuilding = false;
        std::vector<std::function<void()>> mSteps;
        size_t mNextStep = 0;

        Params mRuntime;                       // at kLambdaR/G/B (complete set)
        Params mBuildRuntime;                  // for the set being built
        LLVector3 mSunK, mSkyK, mBuildSunK, mBuildSkyK;
        LLColor3 mWhitePoint = LLColor3::white, mBuildWhitePoint = LLColor3::white;
        std::vector<F32> mTransmittanceData, mIrradianceData;   // the complete set, read back
        // keyed by the name's address: every caller passes a string of static lifetime (literals, setParams' table),
        // so a lookup costs no string build
        mutable std::unordered_map<GLuint, std::unordered_map<const char*, GLint>> mLocations;
    };
}

#endif // WOLF_TK_ATMOSPHERE_H
