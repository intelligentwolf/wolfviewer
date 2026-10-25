/**
 * @file wolftkatmosphere.cpp
 * @brief WolfViewer: takram three-geospatial's precomputed atmosphere (Bruneton's model). See wolftkatmosphere.h.
 *
 * Translated for WolfViewer (IntelligentWolf Ltd, 2026-10-12) from three-geospatial (commit
 * b012ad06d858fc035d88aacfd73f092f93c994e4, MIT, (c) 2024 Shota Matsuda) and Eric Bruneton's
 * precomputed_atmospheric_scattering (commit 34f14e745cff948f4ca3157d1b62a445ffa7286f, BSD-3,
 * (c) 2017 Eric Bruneton; (c) 2008 INRIA). Licence texts: indra/newview/THIRD_PARTY_SKY.txt.
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

#include "wolftkatmosphere.h"

#include <array>
#include <cmath>

#include "llgl.h"
#include "llrender.h"
#include "llvertexbuffer.h"
#include "pipeline.h"
#include "wolfenvironmentstate.h"
#include "wolftkgl.h"

namespace
{
    using namespace WolfTk;

    // Source: ebruneton atmosphere/constants.h:64 MAX_LUMINOUS_EFFICACY (lumen / watt)
    const F64 MAX_LUMINOUS_EFFICACY = 683.0;
    // Source: ebruneton atmosphere/constants.h:69-165 CIE_2_DEG_COLOR_MATCHING_FUNCTIONS (lambda, x, y, z per row)
    // and :169-173 XYZ_TO_SRGB; atmosphere/demo/demo.cc:196-203 kSolarIrradiance (W m^-2 nm^-1, ASTM G-173),
    // :208-216 kOzoneCrossSection (m^2), the 48 values from 360 to 830 nm in steps of 10.
    const F64 CIE_2_DEG_COLOR_MATCHING_FUNCTIONS[380] = {
      360, 0.000129900000, 0.000003917000, 0.000606100000,
      365, 0.000232100000, 0.000006965000, 0.001086000000,
      370, 0.000414900000, 0.000012390000, 0.001946000000,
      375, 0.000741600000, 0.000022020000, 0.003486000000,
      380, 0.001368000000, 0.000039000000, 0.006450001000,
      385, 0.002236000000, 0.000064000000, 0.010549990000,
      390, 0.004243000000, 0.000120000000, 0.020050010000,
      395, 0.007650000000, 0.000217000000, 0.036210000000,
      400, 0.014310000000, 0.000396000000, 0.067850010000,
      405, 0.023190000000, 0.000640000000, 0.110200000000,
      410, 0.043510000000, 0.001210000000, 0.207400000000,
      415, 0.077630000000, 0.002180000000, 0.371300000000,
      420, 0.134380000000, 0.004000000000, 0.645600000000,
      425, 0.214770000000, 0.007300000000, 1.039050100000,
      430, 0.283900000000, 0.011600000000, 1.385600000000,
      435, 0.328500000000, 0.016840000000, 1.622960000000,
      440, 0.348280000000, 0.023000000000, 1.747060000000,
      445, 0.348060000000, 0.029800000000, 1.782600000000,
      450, 0.336200000000, 0.038000000000, 1.772110000000,
      455, 0.318700000000, 0.048000000000, 1.744100000000,
      460, 0.290800000000, 0.060000000000, 1.669200000000,
      465, 0.251100000000, 0.073900000000, 1.528100000000,
      470, 0.195360000000, 0.090980000000, 1.287640000000,
      475, 0.142100000000, 0.112600000000, 1.041900000000,
      480, 0.095640000000, 0.139020000000, 0.812950100000,
      485, 0.057950010000, 0.169300000000, 0.616200000000,
      490, 0.032010000000, 0.208020000000, 0.465180000000,
      495, 0.014700000000, 0.258600000000, 0.353300000000,
      500, 0.004900000000, 0.323000000000, 0.272000000000,
      505, 0.002400000000, 0.407300000000, 0.212300000000,
      510, 0.009300000000, 0.503000000000, 0.158200000000,
      515, 0.029100000000, 0.608200000000, 0.111700000000,
      520, 0.063270000000, 0.710000000000, 0.078249990000,
      525, 0.109600000000, 0.793200000000, 0.057250010000,
      530, 0.165500000000, 0.862000000000, 0.042160000000,
      535, 0.225749900000, 0.914850100000, 0.029840000000,
      540, 0.290400000000, 0.954000000000, 0.020300000000,
      545, 0.359700000000, 0.980300000000, 0.013400000000,
      550, 0.433449900000, 0.994950100000, 0.008749999000,
      555, 0.512050100000, 1.000000000000, 0.005749999000,
      560, 0.594500000000, 0.995000000000, 0.003900000000,
      565, 0.678400000000, 0.978600000000, 0.002749999000,
      570, 0.762100000000, 0.952000000000, 0.002100000000,
      575, 0.842500000000, 0.915400000000, 0.001800000000,
      580, 0.916300000000, 0.870000000000, 0.001650001000,
      585, 0.978600000000, 0.816300000000, 0.001400000000,
      590, 1.026300000000, 0.757000000000, 0.001100000000,
      595, 1.056700000000, 0.694900000000, 0.001000000000,
      600, 1.062200000000, 0.631000000000, 0.000800000000,
      605, 1.045600000000, 0.566800000000, 0.000600000000,
      610, 1.002600000000, 0.503000000000, 0.000340000000,
      615, 0.938400000000, 0.441200000000, 0.000240000000,
      620, 0.854449900000, 0.381000000000, 0.000190000000,
      625, 0.751400000000, 0.321000000000, 0.000100000000,
      630, 0.642400000000, 0.265000000000, 0.000049999990,
      635, 0.541900000000, 0.217000000000, 0.000030000000,
      640, 0.447900000000, 0.175000000000, 0.000020000000,
      645, 0.360800000000, 0.138200000000, 0.000010000000,
      650, 0.283500000000, 0.107000000000, 0.000000000000,
      655, 0.218700000000, 0.081600000000, 0.000000000000,
      660, 0.164900000000, 0.061000000000, 0.000000000000,
      665, 0.121200000000, 0.044580000000, 0.000000000000,
      670, 0.087400000000, 0.032000000000, 0.000000000000,
      675, 0.063600000000, 0.023200000000, 0.000000000000,
      680, 0.046770000000, 0.017000000000, 0.000000000000,
      685, 0.032900000000, 0.011920000000, 0.000000000000,
      690, 0.022700000000, 0.008210000000, 0.000000000000,
      695, 0.015840000000, 0.005723000000, 0.000000000000,
      700, 0.011359160000, 0.004102000000, 0.000000000000,
      705, 0.008110916000, 0.002929000000, 0.000000000000,
      710, 0.005790346000, 0.002091000000, 0.000000000000,
      715, 0.004109457000, 0.001484000000, 0.000000000000,
      720, 0.002899327000, 0.001047000000, 0.000000000000,
      725, 0.002049190000, 0.000740000000, 0.000000000000,
      730, 0.001439971000, 0.000520000000, 0.000000000000,
      735, 0.000999949300, 0.000361100000, 0.000000000000,
      740, 0.000690078600, 0.000249200000, 0.000000000000,
      745, 0.000476021300, 0.000171900000, 0.000000000000,
      750, 0.000332301100, 0.000120000000, 0.000000000000,
      755, 0.000234826100, 0.000084800000, 0.000000000000,
      760, 0.000166150500, 0.000060000000, 0.000000000000,
      765, 0.000117413000, 0.000042400000, 0.000000000000,
      770, 0.000083075270, 0.000030000000, 0.000000000000,
      775, 0.000058706520, 0.000021200000, 0.000000000000,
      780, 0.000041509940, 0.000014990000, 0.000000000000,
      785, 0.000029353260, 0.000010600000, 0.000000000000,
      790, 0.000020673830, 0.000007465700, 0.000000000000,
      795, 0.000014559770, 0.000005257800, 0.000000000000,
      800, 0.000010253980, 0.000003702900, 0.000000000000,
      805, 0.000007221456, 0.000002607800, 0.000000000000,
      810, 0.000005085868, 0.000001836600, 0.000000000000,
      815, 0.000003581652, 0.000001293400, 0.000000000000,
      820, 0.000002522525, 0.000000910930, 0.000000000000,
      825, 0.000001776509, 0.000000641530, 0.000000000000,
      830, 0.000001251141, 0.000000451810, 0.000000000000,
    };
    const F64 XYZ_TO_SRGB[9] = {
      +3.2406, -1.5372, -0.4986,
      -0.9689, +1.8758, +0.0415,
      +0.0557, -0.2040, +1.0570
    };
    const F64 kSolarIrradiance[48] = {
        1.11776, 1.14259, 1.01249, 1.14716, 1.72765, 1.73054, 1.6887, 1.61253,
        1.91198, 2.03474, 2.02042, 2.02212, 1.93377, 1.95809, 1.91686, 1.8298,
        1.8685, 1.8931, 1.85149, 1.8504, 1.8341, 1.8345, 1.8147, 1.78158, 1.7533,
        1.6965, 1.68194, 1.64654, 1.6048, 1.52143, 1.55622, 1.5113, 1.474, 1.4482,
        1.41018, 1.36775, 1.34188, 1.31429, 1.28303, 1.26758, 1.2367, 1.2082,
        1.18737, 1.14683, 1.12362, 1.1058, 1.07124, 1.04992
      };
    const F64 kOzoneCrossSection[48] = {
        1.18e-27, 2.182e-28, 2.818e-28, 6.636e-28, 1.527e-27, 2.763e-27, 5.52e-27,
        8.451e-27, 1.582e-26, 2.316e-26, 3.669e-26, 4.924e-26, 7.752e-26, 9.016e-26,
        1.48e-25, 1.602e-25, 2.139e-25, 2.755e-25, 3.091e-25, 3.5e-25, 4.266e-25,
        4.672e-25, 4.398e-25, 4.701e-25, 5.019e-25, 4.305e-25, 3.74e-25, 3.215e-25,
        2.662e-25, 2.238e-25, 1.852e-25, 1.473e-25, 1.209e-25, 9.423e-26, 7.455e-26,
        6.566e-26, 5.105e-26, 4.15e-26, 4.228e-26, 3.237e-26, 2.451e-26, 2.801e-26,
        2.534e-26, 1.624e-26, 1.465e-26, 2.078e-26, 1.383e-26, 7.105e-27
      };
    // Source: ebruneton atmosphere/model.h:306-308 kLambdaR / kLambdaG / kLambdaB (nm)
    const F64 kLambdaR = 680.0, kLambdaG = 550.0, kLambdaB = 440.0;
    // Source: ebruneton atmosphere/model.cc:518-519 kLambdaMin / kLambdaMax
    const S32 kLambdaMin = 360, kLambdaMax = 830;

    // Source: ebruneton atmosphere/model.cc:521-533 CieColorMatchingFunctionTableValue
    F64 cieColorMatchingFunctionTableValue(F64 wavelength, S32 column)
    {
        if (wavelength <= kLambdaMin || wavelength >= kLambdaMax) return 0.0;
        F64 u = (wavelength - kLambdaMin) / 5.0;
        const S32 row = (S32)std::floor(u);
        llassert(row >= 0 && row + 1 < 95);
        u -= row;
        return CIE_2_DEG_COLOR_MATCHING_FUNCTIONS[4 * row + column] * (1.0 - u) +
               CIE_2_DEG_COLOR_MATCHING_FUNCTIONS[4 * (row + 1) + column] * u;
    }

    // Source: ebruneton atmosphere/model.cc:535-552 Interpolate
    F64 interpolate(const std::vector<F64>& wavelengths, const std::vector<F64>& wavelength_function, F64 wavelength)
    {
        llassert(wavelength_function.size() == wavelengths.size());
        if (wavelength < wavelengths[0]) return wavelength_function[0];
        for (size_t i = 0; i + 1 < wavelengths.size(); ++i)
        {
            if (wavelength < wavelengths[i + 1])
            {
                const F64 u = (wavelength - wavelengths[i]) / (wavelengths[i + 1] - wavelengths[i]);
                return wavelength_function[i] * (1.0 - u) + wavelength_function[i + 1] * u;
            }
        }
        return wavelength_function[wavelength_function.size() - 1];
    }

    // Source: ebruneton atmosphere/model.cc:562-595 ComputeSpectralRadianceToLuminanceFactors (lumen nm / watt)
    void computeSpectralRadianceToLuminanceFactors(const std::vector<F64>& wavelengths, const std::vector<F64>& solar_irradiance,
                                                   F64 lambda_power, F64* k_r, F64* k_g, F64* k_b)
    {
        *k_r = 0.0;
        *k_g = 0.0;
        *k_b = 0.0;
        const F64 solar_r = interpolate(wavelengths, solar_irradiance, kLambdaR);
        const F64 solar_g = interpolate(wavelengths, solar_irradiance, kLambdaG);
        const F64 solar_b = interpolate(wavelengths, solar_irradiance, kLambdaB);
        const S32 dlambda = 1;
        for (S32 lambda = kLambdaMin; lambda < kLambdaMax; lambda += dlambda)
        {
            const F64 x_bar = cieColorMatchingFunctionTableValue(lambda, 1);
            const F64 y_bar = cieColorMatchingFunctionTableValue(lambda, 2);
            const F64 z_bar = cieColorMatchingFunctionTableValue(lambda, 3);
            const F64* xyz2srgb = XYZ_TO_SRGB;
            const F64 r_bar = xyz2srgb[0] * x_bar + xyz2srgb[1] * y_bar + xyz2srgb[2] * z_bar;
            const F64 g_bar = xyz2srgb[3] * x_bar + xyz2srgb[4] * y_bar + xyz2srgb[5] * z_bar;
            const F64 b_bar = xyz2srgb[6] * x_bar + xyz2srgb[7] * y_bar + xyz2srgb[8] * z_bar;
            const F64 irradiance = interpolate(wavelengths, solar_irradiance, lambda);
            *k_r += r_bar * irradiance / solar_r * pow(lambda / kLambdaR, lambda_power);
            *k_g += g_bar * irradiance / solar_g * pow(lambda / kLambdaG, lambda_power);
            *k_b += b_bar * irradiance / solar_b * pow(lambda / kLambdaB, lambda_power);
        }
        *k_r *= MAX_LUMINOUS_EFFICACY * dlambda;
        *k_g *= MAX_LUMINOUS_EFFICACY * dlambda;
        *k_b *= MAX_LUMINOUS_EFFICACY * dlambda;
    }

    /** Source: ebruneton atmosphere/model.cc:1020-1040 Model::ConvertSpectrumToLinearSrgb, normalised to a mean of 1
     *  as demo.cc:331-337 does for its white balance (the sun's colour above the atmosphere). */
    LLColor3 solarWhitePoint(const std::vector<F64>& wavelengths, const std::vector<F64>& spectrum)
    {
        F64 x = 0.0, y = 0.0, z = 0.0;
        const S32 dlambda = 1;
        for (S32 lambda = kLambdaMin; lambda < kLambdaMax; lambda += dlambda)
        {
            const F64 value = interpolate(wavelengths, spectrum, lambda);
            x += cieColorMatchingFunctionTableValue(lambda, 1) * value;
            y += cieColorMatchingFunctionTableValue(lambda, 2) * value;
            z += cieColorMatchingFunctionTableValue(lambda, 3) * value;
        }
        const F64 r = MAX_LUMINOUS_EFFICACY * (XYZ_TO_SRGB[0] * x + XYZ_TO_SRGB[1] * y + XYZ_TO_SRGB[2] * z) * dlambda;
        const F64 g = MAX_LUMINOUS_EFFICACY * (XYZ_TO_SRGB[3] * x + XYZ_TO_SRGB[4] * y + XYZ_TO_SRGB[5] * z) * dlambda;
        const F64 b = MAX_LUMINOUS_EFFICACY * (XYZ_TO_SRGB[6] * x + XYZ_TO_SRGB[7] * y + XYZ_TO_SRGB[8] * z) * dlambda;
        const F64 mean = (r + g + b) / 3.0;
        if (!(mean > 0.0)) return LLColor3::white;
        return LLColor3((F32)(r / mean), (F32)(g / mean), (F32)(b / mean));
    }

    /** The atmosphere at three wavelengths in the shader's length unit (km). Source: ebruneton
     *  atmosphere/model.cc:636-666 to_string / density_layer / density_profile and :718-734 the ATMOSPHERE constant
     *  (length_unit_in_meters = 1000, demo.cc:72 kLengthUnitInMeters). */
    Atmosphere::Params paramsAt(const AtmosphereSpectrum& s, const F64 lambdas[3])
    {
        const F64 unit = 1.0 / METER_TO_LENGTH_UNIT;
        Atmosphere::Params p;
        for (S32 i = 0; i < 3; ++i)
        {
            p.solar_irradiance[i] = interpolate(s.wavelengths, s.solar_irradiance, lambdas[i]) * 1.0;
            p.rayleigh_scattering[i] = interpolate(s.wavelengths, s.rayleigh_scattering, lambdas[i]) * unit;
            p.mie_scattering[i] = interpolate(s.wavelengths, s.mie_scattering, lambdas[i]) * unit;
            p.mie_extinction[i] = interpolate(s.wavelengths, s.mie_extinction, lambdas[i]) * unit;
            p.absorption_extinction[i] = interpolate(s.wavelengths, s.absorption_extinction, lambdas[i]) * unit;
            p.ground_albedo[i] = interpolate(s.wavelengths, s.ground_albedo, lambdas[i]) * 1.0;
        }
        auto layer = [unit](const DensityProfileLayer& l)
        {
            // model.cc:644-652 density_layer: width / unit, exp_scale x unit, linear_term x unit
            DensityProfileLayer o;
            o.width = l.width / unit;
            o.exp_term = l.exp_term;
            o.exp_scale = l.exp_scale * unit;
            o.linear_term = l.linear_term * unit;
            o.constant_term = l.constant_term;
            return o;
        };
        for (S32 i = 0; i < 2; ++i)
        {
            p.rayleigh_density[i] = layer(s.rayleigh_density[i]);
            p.mie_density[i] = layer(s.mie_density[i]);
            p.absorption_density[i] = layer(s.absorption_density[i]);
        }
        p.sun_angular_radius = s.sun_angular_radius;
        p.bottom_radius = s.bottom_radius / unit;
        p.top_radius = s.top_radius / unit;
        p.mie_phase_function_g = s.mie_phase_function_g;
        p.mu_s_min = cos(s.max_sun_zenith_angle);   // model.cc:734
        return p;
    }

    // ---- CPU reads of the tables ------------------------------------------------------------------------

    // Source: three-geospatial packages/atmosphere/src/helpers/functions.ts:3-35 (safeSqrt, clampDistance,
    // rayIntersectsGround, distanceToTopAtmosphereBoundary, getTextureCoordFromUnitRange)
    F64 safeSqrt(F64 a) { return sqrt(llmax(a, 0.0)); }
    F64 clampDistance(F64 d) { return llmax(d, 0.0); }
    bool rayIntersectsGround(const Atmosphere::Params& a, F64 r, F64 mu)
    {
        return mu < 0.0 && r * r * (mu * mu - 1.0) + a.bottom_radius * a.bottom_radius >= 0.0;
    }
    F64 distanceToTopAtmosphereBoundary(const Atmosphere::Params& a, F64 r, F64 mu)
    {
        const F64 discriminant = r * r * (mu * mu - 1.0) + a.top_radius * a.top_radius;
        return clampDistance(-r * mu + safeSqrt(discriminant));
    }
    F64 getTextureCoordFromUnitRange(F64 x, S32 texture_size) { return 0.5 / texture_size + x * (1.0 - 1.0 / texture_size); }

    // Source: three-geospatial packages/atmosphere/src/getSunLightColor.ts:18-36 getUvFromRMu
    void getUvFromRMu(const Atmosphere::Params& a, F64 r, F64 mu, F64& u, F64& v)
    {
        const F64 H = sqrt(a.top_radius * a.top_radius - a.bottom_radius * a.bottom_radius);
        const F64 rho = safeSqrt(r * r - a.bottom_radius * a.bottom_radius);
        const F64 d = distanceToTopAtmosphereBoundary(a, r, mu);
        const F64 dMin = a.top_radius - r;
        const F64 dMax = rho + H;
        const F64 xmu = (d - dMin) / (dMax - dMin);
        const F64 xr = rho / H;
        u = getTextureCoordFromUnitRange(xmu, TRANSMITTANCE_TEXTURE_WIDTH);
        v = getTextureCoordFromUnitRange(xr, TRANSMITTANCE_TEXTURE_HEIGHT);
    }

    // Source: three-geospatial packages/atmosphere/src/SkyLightProbe.ts:21-33 getUvFromRMuS
    void getUvFromRMuS(const Atmosphere::Params& a, F64 r, F64 muS, F64& u, F64& v)
    {
        const F64 xR = (r - a.bottom_radius) / (a.top_radius - a.bottom_radius);
        const F64 xMuS = muS * 0.5 + 0.5;
        u = getTextureCoordFromUnitRange(xMuS, IRRADIANCE_TEXTURE_WIDTH);
        v = getTextureCoordFromUnitRange(xR, IRRADIANCE_TEXTURE_HEIGHT);
    }

    // Source: three-geospatial packages/atmosphere/src/helpers/sampleTexture.ts:51-82 sampleTexture (and
    // samplePixel :43-50): bilinear over texel corners, x = clamp(u, 0, 1) (width - 1), RGBA rows.
    LLVector3 sampleTexture(const std::vector<F32>& data, S32 width, S32 height, F64 uu, F64 vv)
    {
        if (data.empty()) return LLVector3::zero;
        const F64 x = llclamp(uu, 0.0, 1.0) * (width - 1);
        const F64 y = llclamp(vv, 0.0, 1.0) * (height - 1);
        const S32 xi = (S32)std::floor(x);
        const S32 yi = (S32)std::floor(y);
        const F64 tx = x - xi;
        const F64 ty = y - yi;
        const S32 rx0 = xi % width;
        const S32 rx1 = (rx0 + 1) % width;
        const S32 ry0 = yi % height;
        const S32 ry1 = (ry0 + 1) % height;
        auto px = [&](S32 index) { const size_t i = (size_t)index * 4; return LLVector3(data[i], data[i + 1], data[i + 2]); };
        const LLVector3 v00 = px(ry0 * width + rx0), v10 = px(ry0 * width + rx1);
        const LLVector3 nx0 = lerp(v00, v10, (F32)tx);
        const LLVector3 v01 = px(ry1 * width + rx0), v11 = px(ry1 * width + rx1);
        const LLVector3 nx1 = lerp(v01, v11, (F32)tx);
        return lerp(nx0, nx1, (F32)ty);
    }

    /** GLSL texture() of a LINEAR, CLAMP_TO_EDGE 2D texture (OpenGL 4.6 core spec 8.14.2: texel centres at
     *  (i + 0.5) / size), for the CPU copies of the runtime's GLSL functions below. */
    LLVector3 textureLinear(const std::vector<F32>& data, S32 width, S32 height, F64 uu, F64 vv)
    {
        if (data.empty()) return LLVector3::zero;
        const F64 x = uu * width - 0.5, y = vv * height - 0.5;
        const S32 x0 = (S32)std::floor(x), y0 = (S32)std::floor(y);
        const F64 fx = x - x0, fy = y - y0;
        auto px = [&](S32 i, S32 j)
        {
            i = llclamp(i, 0, width - 1); j = llclamp(j, 0, height - 1);
            const size_t k = ((size_t)j * width + i) * 4;
            return LLVector3(data[k], data[k + 1], data[k + 2]);
        };
        const LLVector3 a = lerp(px(x0, y0), px(x0 + 1, y0), (F32)fx);
        const LLVector3 b = lerp(px(x0, y0 + 1), px(x0 + 1, y0 + 1), (F32)fx);
        return lerp(a, b, (F32)fy);
    }

    // Source: three-geospatial packages/atmosphere/src/shaders/bruneton/common.glsl:106-149
    // GetTransmittanceTextureUvFromRMu / GetTransmittanceToTopAtmosphereBoundary (float tables: no log branch)
    LLVector3 transmittanceToTop(const Atmosphere::Params& a, const std::vector<F32>& t, F64 r, F64 mu)
    {
        F64 u, v;
        getUvFromRMu(a, r, mu, u, v);   // the same expressions as common.glsl:106-125
        return textureLinear(t, TRANSMITTANCE_TEXTURE_WIDTH, TRANSMITTANCE_TEXTURE_HEIGHT, u, v);
    }

    // Source: three-geospatial packages/atmosphere/src/shaders/bruneton/common.glsl:180-191 GetTransmittanceToSun
    LLVector3 transmittanceToSun(const Atmosphere::Params& a, const std::vector<F32>& t, F64 r, F64 mu_s)
    {
        const F64 sin_theta_h = a.bottom_radius / r;
        const F64 cos_theta_h = -sqrt(llmax(1.0 - sin_theta_h * sin_theta_h, 0.0));
        const F64 e0 = -sin_theta_h * a.sun_angular_radius, e1 = sin_theta_h * a.sun_angular_radius;
        const F64 x = llclamp((mu_s - cos_theta_h - e0) / (e1 - e0), 0.0, 1.0);
        const F64 smooth = x * x * (3.0 - 2.0 * x);   // GLSL smoothstep
        return transmittanceToTop(a, t, r, mu_s) * (F32)smooth;
    }

    // Source: three-geospatial packages/atmosphere/src/shaders/bruneton/common.glsl:263-280
    // GetIrradianceTextureUvFromRMuS / GetIrradiance
    LLVector3 getIrradiance(const Atmosphere::Params& a, const std::vector<F32>& irr, F64 r, F64 mu_s)
    {
        F64 u, v;
        getUvFromRMuS(a, r, mu_s, u, v);   // the same expressions as common.glsl:263-272
        return textureLinear(irr, IRRADIANCE_TEXTURE_WIDTH, IRRADIANCE_TEXTURE_HEIGHT, u, v);
    }

    LLColor3 mul(const LLVector3& a, const LLVector3& b) { return LLColor3(a.mV[0] * b.mV[0], a.mV[1] * b.mV[1], a.mV[2] * b.mV[2]); }

    GLuint newTexture2d(S32 w, S32 h)
    {
        // Source: three-geospatial PrecomputedTexturesGenerator.ts:70-87 createRenderTarget (RGBA, float, linear,
        // clamp to edge, no depth); FloatType when float linear filtering is supported (:384) - core in desktop GL.
        GLuint t = 0;
        glGenTextures(1, &t);
        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        return t;
    }

    GLuint newTexture3d(S32 w, S32 h, S32 d)
    {
        // Source: three-geospatial PrecomputedTexturesGenerator.ts:89-108 create3DRenderTarget
        GLuint t = 0;
        glGenTextures(1, &t);
        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE_3D, t);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA32F, w, h, d, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE_3D);
        return t;
    }

    void deleteTexture(GLuint& t)
    {
        if (t) glDeleteTextures(1, &t);
        t = 0;
    }

    /** The draw state of one precompute pass: no depth, no culling, no scissor, all four channels written (the
     *  scattering table keeps Mie's red in alpha, definitions.glsl / model.cc combined textures, and LL often
     *  masks alpha writes), PrecomputeMaterial.additive blending (PrecomputedTexturesGenerator.ts:231-240: One /
     *  One, add) set per draw; the previous framebuffer, viewport, colour mask and bound program restored. */
    struct PassScope
    {
        GLint prevFbo = 0;
        GLint prevViewport[4] = {};
        GLboolean prevMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
        LLGLSLShader* prevShader = nullptr;
        LLGLDepthTest depth{ GL_FALSE, GL_FALSE };
        LLGLDisable cull{ GL_CULL_FACE };
        LLGLDisable scissor{ GL_SCISSOR_TEST };
        PassScope()
        {
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevFbo);
            glGetIntegerv(GL_VIEWPORT, prevViewport);
            glGetBooleanv(GL_COLOR_WRITEMASK, prevMask);
            prevShader = LLGLSLShader::sCurBoundShaderPtr;
            gGL.setColorMask(true, true);
        }
        ~PassScope()
        {
            gGL.setColorMask(prevMask[0] == GL_TRUE, prevMask[1] == GL_TRUE, prevMask[2] == GL_TRUE, prevMask[3] == GL_TRUE);
            glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
            glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
            if (prevShader && LLGLSLShader::sCurBoundShaderPtr != prevShader) prevShader->bind();
        }
    };

    void drawScreen(bool additive)
    {
        if (additive)
        {
            LLGLEnable blend(GL_BLEND);
            gGL.blendFunc(LLRender::BF_ONE, LLRender::BF_ONE);
            gPipeline.mScreenTriangleVB->setBuffer();
            gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
            // LL's default blend, as every LL pass leaves it (the steps run from WolfEnvironmentRender::frameUpdate,
            // LLDrawPoolWLSky's frame start, where that is the state; LLRender keeps its blend factors private)
            gGL.setSceneBlendType(LLRender::BT_ALPHA);
        }
        else
        {
            LLGLDisable blend(GL_BLEND);
            gPipeline.mScreenTriangleVB->setBuffer();
            gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        }
    }
}

namespace WolfTk
{

// Source: ebruneton atmosphere/demo/demo.cc:188-284 Demo::InitModel (use_constant_solar_spectrum_ false, use_ozone_
// true); sun angular radius demo.cc:70 kSunAngularRadius = 0.00935 / 2 = three-geospatial
// AtmosphereParameters.ts:102 sunAngularRadius 0.004675; maximum sun zenith 120 degrees (demo.cc:236-237 at full
// precision; = three-geospatial AtmosphereParameters.ts:170 muSMin = cos(radians(120))).
AtmosphereSpectrum AtmosphereSpectrum::earth()
{
    AtmosphereSpectrum s;
    const F64 kDobsonUnit = 2.687e20;                                  // demo.cc:218
    const F64 kMaxOzoneNumberDensity = 300.0 * kDobsonUnit / 15000.0;  // demo.cc:222
    const F64 kBottomRadius = 6360000.0;                               // demo.cc:226
    const F64 kTopRadius = 6420000.0;                                  // demo.cc:227
    const F64 kRayleigh = 1.24062e-6;                                  // demo.cc:228
    const F64 kRayleighScaleHeight = 8000.0;                           // demo.cc:229
    const F64 kMieScaleHeight = 1200.0;                                // demo.cc:230
    // [WolfViewer] demo.cc:231-232 kMieAngstromAlpha 0, kMieAngstromBeta 5.328e-3: an aerosol optical depth of 0.005
    // at every wavelength, an exceptionally clean sky (zenith 1717 cd/m2 at sun 60 deg, against 5900 cd/m2 for
    // Karayel et al. 1984's turbidity-2 clear sky). WolfViewer's Earth: tau(550 nm) = 0.13, the global mean over the
    // oceans (Remer et al. 2008, J. Geophys. Res. 113, D14S07, MODIS Aqua Collection 5), with the oceanic Angstrom
    // exponent 0.6 (Smirnov et al. 2009, J. Geophys. Res. 114, D06204, Maritime Aerosol Network) for its spectral
    // shape. In demo.cc's form mie = beta / H x lambda^-alpha (lambda in um, :260-262), tau(lambda) = beta
    // lambda^-alpha, so beta = 0.13 x 0.55^0.6 (tau at 1 um). The scale height, single-scattering albedo and phase g
    // stay demo.cc's.
    const F64 kMieAngstromAlpha = 0.6;
    const F64 kMieAngstromBeta = 0.13 * pow(0.55, 0.6);
    const F64 kMieSingleScatteringAlbedo = 0.9;                        // demo.cc:233
    const F64 kMiePhaseFunctionG = 0.8;                                // demo.cc:234
    const F64 kGroundAlbedo = 0.1;                                     // demo.cc:235
    s.sun_angular_radius = 0.00935 / 2.0;
    s.bottom_radius = kBottomRadius;
    s.top_radius = kTopRadius;
    s.mie_phase_function_g = kMiePhaseFunctionG;
    s.max_sun_zenith_angle = 120.0 / 180.0 * F_PI;
    // demo.cc:239-241 rayleigh_layer / mie_layer (one layer each; model.cc:655-658 puts an empty one below it)
    s.rayleigh_density[1] = { 0.0, 1.0, -1.0 / kRayleighScaleHeight, 0.0, 0.0 };
    s.mie_density[1] = { 0.0, 1.0, -1.0 / kMieScaleHeight, 0.0, 0.0 };
    // demo.cc:242-250 the ozone profile: up from 0 to 1 between 10 and 25 km, down to 0 at 40 km
    s.absorption_density[0] = { 25000.0, 0.0, 0.0, 1.0 / 15000.0, -2.0 / 3.0 };
    s.absorption_density[1] = { 0.0, 0.0, 0.0, -1.0 / 15000.0, 8.0 / 3.0 };
    // demo.cc:259-276
    for (S32 l = kLambdaMin; l <= kLambdaMax; l += 10)
    {
        const F64 lambda = (F64)l * 1e-3;   // micrometres
        const F64 mie = kMieAngstromBeta / kMieScaleHeight * pow(lambda, -kMieAngstromAlpha);
        s.wavelengths.push_back(l);
        s.solar_irradiance.push_back(kSolarIrradiance[(l - kLambdaMin) / 10]);
        s.rayleigh_scattering.push_back(kRayleigh * pow(lambda, -4));
        s.mie_scattering.push_back(mie * kMieSingleScatteringAlbedo);
        s.mie_extinction.push_back(mie);
        s.absorption_extinction.push_back(kMaxOzoneNumberDensity * kOzoneCrossSection[(l - kLambdaMin) / 10]);
        s.ground_albedo.push_back(kGroundAlbedo);
    }
    return s;
}

// [WolfViewer] The Wolf Environment document's sky block (wolfenvironmentstate.cpp:160-168) on Earth's spectrum, the
// same mapping the previous native sky used (wolfenvironmentrender.cpp before this translation: tinted() :267-274,
// ratioAt() :347-358, updateAtmosphere() :888-898): a colour (display sRGB, linearised) is scaled so its brightest
// channel has Earth's brightest coefficient; its ratio to Earth at 440 / 550 / 680 nm is carried to every
// wavelength linearly between and flat beyond (model.cc:535-552 Interpolate semantics); the densities multiply;
// the planet and atmosphere sizes go across directly; Mie g is the document's.
AtmosphereSpectrum AtmosphereSpectrum::fromDoc(const WolfEnvDoc& d)
{
    AtmosphereSpectrum s = earth();
    const F64 lam[3] = { kLambdaR, kLambdaG, kLambdaB };
    auto ratios = [&](const std::vector<F64>& earth_coeff, bool has, const LLColor3& colour, F64 out[3])
    {
        out[0] = out[1] = out[2] = 1.0;
        if (!has) return;
        const LLColor3 c = linearColor3(colour);
        F64 e[3];
        for (S32 i = 0; i < 3; ++i) e[i] = interpolate(s.wavelengths, earth_coeff, lam[i]);
        const F64 top = llmax(c.mV[0], llmax(c.mV[1], c.mV[2]));
        const F64 etop = llmax(e[0], llmax(e[1], e[2]));
        for (S32 i = 0; i < 3; ++i) out[i] = (top > 0.0 && e[i] > 0.0) ? (c.mV[i] * (etop / top)) / e[i] : 0.0;
    };
    // the ratio at wavelength l from the three at 440 / 550 / 680 nm (index 2, 1, 0)
    auto ratioAt = [](const F64 r[3], F64 l)
    {
        const std::vector<F64> wl = { kLambdaB, kLambdaG, kLambdaR };
        const std::vector<F64> v = { r[2], r[1], r[0] };
        return interpolate(wl, v, l);
    };
    F64 rr[3], rm[3], ro[3];
    ratios(s.rayleigh_scattering, d.mHasRayleigh, d.mRayleigh, rr);
    ratios(s.mie_extinction, d.mHasMie, d.mMie, rm);
    ratios(s.absorption_extinction, d.mHasOzone, d.mOzone, ro);
    for (size_t i = 0; i < s.wavelengths.size(); ++i)
    {
        const F64 l = s.wavelengths[i];
        s.rayleigh_scattering[i] *= ratioAt(rr, l) * d.mRayleighDensity;
        s.mie_scattering[i] *= ratioAt(rm, l) * d.mMieDensity;
        s.mie_extinction[i] *= ratioAt(rm, l) * d.mMieDensity;
        s.absorption_extinction[i] *= ratioAt(ro, l) * d.mOzoneDensity;
    }
    s.mie_phase_function_g = llclamp((F64)d.mMieAnisotropy, 0.0, 0.95);
    s.bottom_radius = (F64)d.mPlanetKm * 1000.0;
    s.top_radius = ((F64)d.mPlanetKm + (F64)d.mAtmosphereKm) * 1000.0;
    return s;
}

size_t hashOf(const AtmosphereSpectrum& s)
{
    size_t h = 1469598103934665603ull;
    auto mix = [&h](F64 v) { h = (h ^ std::hash<F64>()(v)) * 1099511628211ull; };
    for (const std::vector<F64>* v : { &s.wavelengths, &s.solar_irradiance, &s.rayleigh_scattering, &s.mie_scattering, &s.mie_extinction,
                                       &s.absorption_extinction, &s.ground_albedo })
    {
        mix((F64)v->size());
        for (F64 x : *v) mix(x);
    }
    for (F64 x : { s.sun_angular_radius, s.bottom_radius, s.top_radius, s.mie_phase_function_g, s.max_sun_zenith_angle }) mix(x);
    for (const DensityProfileLayer* p : { s.rayleigh_density, s.mie_density, s.absorption_density })
    {
        for (S32 i = 0; i < 2; ++i)
        {
            mix(p[i].width); mix(p[i].exp_term); mix(p[i].exp_scale); mix(p[i].linear_term); mix(p[i].constant_term);
        }
    }
    return h;
}

// static
void Atmosphere::addDefines(LLGLSLShader& sh)
{
    // Source: three-geospatial AtmosphereMaterialBase.ts:147-158 (= AerialPerspectiveEffect.ts:291-301): PI as
    // `${Math.PI}`, the sizes as integers, METER_TO_LENGTH_UNIT.toFixed(7)
    sh.addPermutation("PI", "3.141592653589793");
    sh.addPermutation("TRANSMITTANCE_TEXTURE_WIDTH", llformat("%d", TRANSMITTANCE_TEXTURE_WIDTH));
    sh.addPermutation("TRANSMITTANCE_TEXTURE_HEIGHT", llformat("%d", TRANSMITTANCE_TEXTURE_HEIGHT));
    sh.addPermutation("SCATTERING_TEXTURE_R_SIZE", llformat("%d", SCATTERING_TEXTURE_R_SIZE));
    sh.addPermutation("SCATTERING_TEXTURE_MU_SIZE", llformat("%d", SCATTERING_TEXTURE_MU_SIZE));
    sh.addPermutation("SCATTERING_TEXTURE_MU_S_SIZE", llformat("%d", SCATTERING_TEXTURE_MU_S_SIZE));
    sh.addPermutation("SCATTERING_TEXTURE_NU_SIZE", llformat("%d", SCATTERING_TEXTURE_NU_SIZE));
    sh.addPermutation("IRRADIANCE_TEXTURE_WIDTH", llformat("%d", IRRADIANCE_TEXTURE_WIDTH));
    sh.addPermutation("IRRADIANCE_TEXTURE_HEIGHT", llformat("%d", IRRADIANCE_TEXTURE_HEIGHT));
    sh.addPermutation("METER_TO_LENGTH_UNIT", "0.0010000");
    // AtmosphereMaterialBase.ts:240-263: combinedScatteringTextures = (singleMieScatteringTexture == null) - the
    // tables are combined (PrecomputedTexturesGenerator.ts:385 combinedScattering = true);
    // hasHigherOrderScatteringTexture = (higherOrderScatteringTexture != null) - it is built (:386). @define sets '1'
    // (packages/core/src/decorators.ts:14-31).
    sh.addPermutation("COMBINED_SCATTERING_TEXTURES", "1");
    sh.addPermutation("HAS_HIGHER_ORDER_SCATTERING_TEXTURE", "1");
}

bool Atmosphere::loadShaders(S32 shader_level)
{
    unloadShaders();
    // One program per precompute pass and OUTPUT (PrecomputedTexturesGenerator.ts:283-371 materials; OUTPUT set per
    // call at :476, :502, :553, :573). WOLF_TK_PRECOMPUTE: 1 transmittance, 2 direct irradiance, 3 single scattering,
    // 4 scattering density, 5 indirect irradiance, 6 multiple scattering (wolftk/precomputeF.glsl).
    struct Def { Pass pass; const char* name; S32 n; const char* output; };
    const Def defs[PASS_COUNT] = {
        { TRANS, "WolfTk Transmittance", 1, nullptr },
        { DIRECT_DELTA, "WolfTk Direct Irradiance (delta)", 2, "deltaIrradiance" },
        { DIRECT_IRR, "WolfTk Direct Irradiance", 2, "irradiance" },
        { SINGLE_RAY, "WolfTk Single Scattering (Rayleigh)", 3, "deltaRayleigh" },
        { SINGLE_MIE, "WolfTk Single Scattering (Mie)", 3, "deltaMie" },
        { SINGLE_SCAT, "WolfTk Single Scattering", 3, "scattering" },
        { DENSITY, "WolfTk Scattering Density", 4, nullptr },
        { INDIRECT_DELTA, "WolfTk Indirect Irradiance (delta)", 5, "deltaIrradiance" },
        { INDIRECT_IRR, "WolfTk Indirect Irradiance", 5, "irradiance" },
        { MULTI_DELTA, "WolfTk Multiple Scattering (delta)", 6, "deltaMultipleScattering" },
        { MULTI_SCAT, "WolfTk Multiple Scattering", 6, "scattering" },
    };
    bool ok = true;
    for (const Def& d : defs)
    {
        LLGLSLShader& sh = mPrograms[d.pass];
        sh.mName = d.name;
        sh.mShaderFiles.clear();
        sh.clearPermutations();
        sh.mShaderFiles.push_back(std::make_pair("wolftk/screenV.glsl", GL_VERTEX_SHADER));
        sh.mShaderFiles.push_back(std::make_pair("wolftk/precomputeF.glsl", GL_FRAGMENT_SHADER));
        sh.mShaderLevel = shader_level;
        // Source: PrecomputedTexturesGenerator.ts:210-228 PrecomputeMaterial defines (the sizes only)
        sh.addPermutation("TRANSMITTANCE_TEXTURE_WIDTH", llformat("%d", TRANSMITTANCE_TEXTURE_WIDTH));
        sh.addPermutation("TRANSMITTANCE_TEXTURE_HEIGHT", llformat("%d", TRANSMITTANCE_TEXTURE_HEIGHT));
        sh.addPermutation("SCATTERING_TEXTURE_R_SIZE", llformat("%d", SCATTERING_TEXTURE_R_SIZE));
        sh.addPermutation("SCATTERING_TEXTURE_MU_SIZE", llformat("%d", SCATTERING_TEXTURE_MU_SIZE));
        sh.addPermutation("SCATTERING_TEXTURE_MU_S_SIZE", llformat("%d", SCATTERING_TEXTURE_MU_S_SIZE));
        sh.addPermutation("SCATTERING_TEXTURE_NU_SIZE", llformat("%d", SCATTERING_TEXTURE_NU_SIZE));
        sh.addPermutation("IRRADIANCE_TEXTURE_WIDTH", llformat("%d", IRRADIANCE_TEXTURE_WIDTH));
        sh.addPermutation("IRRADIANCE_TEXTURE_HEIGHT", llformat("%d", IRRADIANCE_TEXTURE_HEIGHT));
        sh.addPermutation("WOLF_TK_PRECOMPUTE", llformat("%d", d.n));
        if (d.output) sh.addPermutation("OUTPUT", d.output);
        if (!sh.createShader())
        {
            LL_WARNS("WolfEnv") << "precompute program failed: " << d.name << LL_ENDL;
            ok = false;
        }
    }
    mShadersLoaded = ok;
    mLocations.clear();
    return ok;
}

void Atmosphere::unloadShaders()
{
    // a build in flight cannot go on without its programs: cancelled (the caller asks again once they are back)
    if (mBuilding)
    {
        mBuilding = false;
        mSteps.clear();
        mNextStep = 0;
        freeContext();
    }
    for (LLGLSLShader& sh : mPrograms) sh.unload();
    mShadersLoaded = false;
    mLocations.clear();
}

GLint Atmosphere::location(GLuint program, const char* name) const
{
    // glGetUniformLocation on the program itself: LLGLSLShader::mapUniform cuts a name at its first "[0]"
    // (llglslshader.cpp), which would fold "ATMOSPHERE.rayleigh_density.layers[0].width" into
    // "ATMOSPHERE.rayleigh_density.layers"; the struct members are looked up here, once per program.
    auto& m = mLocations[program];
    auto it = m.find(name);
    if (it != m.end()) return it->second;
    const GLint loc = glGetUniformLocation(program, name);
    m[name] = loc;
    return loc;
}

// Source: three-geospatial AtmosphereParameters.ts:194-217 toUniform (the struct of definitions.glsl:128-143)
void Atmosphere::setParams(GLuint program, const Params& p) const
{
    auto v3 = [&](const char* n, const F64 v[3]) { const GLint l = location(program, n); if (l >= 0) glUniform3f(l, (F32)v[0], (F32)v[1], (F32)v[2]); };
    auto f1 = [&](const char* n, F64 v) { const GLint l = location(program, n); if (l >= 0) glUniform1f(l, (F32)v); };
    v3("ATMOSPHERE.solar_irradiance", p.solar_irradiance);
    f1("ATMOSPHERE.sun_angular_radius", p.sun_angular_radius);
    f1("ATMOSPHERE.bottom_radius", p.bottom_radius);
    f1("ATMOSPHERE.top_radius", p.top_radius);
    v3("ATMOSPHERE.rayleigh_scattering", p.rayleigh_scattering);
    v3("ATMOSPHERE.mie_scattering", p.mie_scattering);
    v3("ATMOSPHERE.mie_extinction", p.mie_extinction);
    f1("ATMOSPHERE.mie_phase_function_g", p.mie_phase_function_g);
    v3("ATMOSPHERE.absorption_extinction", p.absorption_extinction);
    v3("ATMOSPHERE.ground_albedo", p.ground_albedo);
    f1("ATMOSPHERE.mu_s_min", p.mu_s_min);
    // the 30 member names, built once (this runs on every bind, every frame)
    static const std::array<std::array<std::array<std::string, 5>, 2>, 3> names = []()
    {
        const char* profiles[3] = { "rayleigh_density", "mie_density", "absorption_density" };
        const char* members[5] = { "width", "exp_term", "exp_scale", "linear_term", "constant_term" };
        std::array<std::array<std::array<std::string, 5>, 2>, 3> out;
        for (S32 k = 0; k < 3; ++k)
            for (S32 i = 0; i < 2; ++i)
                for (S32 m = 0; m < 5; ++m)
                    out[k][i][m] = llformat("ATMOSPHERE.%s.layers[%d].%s", profiles[k], i, members[m]);
        return out;
    }();
    const DensityProfileLayer* layers[3] = { p.rayleigh_density, p.mie_density, p.absorption_density };
    for (S32 k = 0; k < 3; ++k)
    {
        for (S32 i = 0; i < 2; ++i)
        {
            const DensityProfileLayer& L = layers[k][i];
            f1(names[k][i][0].c_str(), L.width);
            f1(names[k][i][1].c_str(), L.exp_term);
            f1(names[k][i][2].c_str(), L.exp_scale);
            f1(names[k][i][3].c_str(), L.linear_term);
            f1(names[k][i][4].c_str(), L.constant_term);
        }
    }
}

void Atmosphere::allocateSet(Set& s)
{
    // Source: PrecomputedTexturesGenerator.ts:392-423 (transmittance, scattering, irradiance, higher-order scattering;
    // no single Mie table: combinedScattering)
    if (!s.transmittance) s.transmittance = newTexture2d(TRANSMITTANCE_TEXTURE_WIDTH, TRANSMITTANCE_TEXTURE_HEIGHT);
    if (!s.scattering) s.scattering = newTexture3d(SCATTERING_TEXTURE_WIDTH, SCATTERING_TEXTURE_HEIGHT, SCATTERING_TEXTURE_DEPTH);
    if (!s.irradiance) s.irradiance = newTexture2d(IRRADIANCE_TEXTURE_WIDTH, IRRADIANCE_TEXTURE_HEIGHT);
    if (!s.higher) s.higher = newTexture3d(SCATTERING_TEXTURE_WIDTH, SCATTERING_TEXTURE_HEIGHT, SCATTERING_TEXTURE_DEPTH);
}

void Atmosphere::freeSet(Set& s)
{
    deleteTexture(s.transmittance);
    deleteTexture(s.scattering);
    deleteTexture(s.irradiance);
    deleteTexture(s.higher);
}

void Atmosphere::allocateContext()
{
    // Source: PrecomputedTexturesGenerator.ts:152-199 Context (float: no opticalDepth target, :163-169);
    // deltaMultipleScattering shares deltaRayleighScattering's texture (:193-198)
    if (!mDeltaIrradiance) mDeltaIrradiance = newTexture2d(IRRADIANCE_TEXTURE_WIDTH, IRRADIANCE_TEXTURE_HEIGHT);
    if (!mDeltaRayleigh) mDeltaRayleigh = newTexture3d(SCATTERING_TEXTURE_WIDTH, SCATTERING_TEXTURE_HEIGHT, SCATTERING_TEXTURE_DEPTH);
    if (!mDeltaMie) mDeltaMie = newTexture3d(SCATTERING_TEXTURE_WIDTH, SCATTERING_TEXTURE_HEIGHT, SCATTERING_TEXTURE_DEPTH);
    if (!mDeltaDensity) mDeltaDensity = newTexture3d(SCATTERING_TEXTURE_WIDTH, SCATTERING_TEXTURE_HEIGHT, SCATTERING_TEXTURE_DEPTH);
    if (!mFbo) glGenFramebuffers(1, &mFbo);
}

void Atmosphere::freeContext()
{
    // Source: PrecomputedTexturesGenerator.ts:201-207 Context.dispose, :740 after the precompute
    deleteTexture(mDeltaIrradiance);
    deleteTexture(mDeltaRayleigh);
    deleteTexture(mDeltaMie);
    deleteTexture(mDeltaDensity);
    if (mFbo) glDeleteFramebuffers(1, &mFbo);
    mFbo = 0;
}

bool Atmosphere::beginPrecompute(const AtmosphereSpectrum& spectrum, U32 num_wavelengths)
{
    if (!mShadersLoaded || !gPipeline.mScreenTriangleVB) return false;
    mSpectrum = spectrum;
    mNumWavelengths = llmax(num_wavelengths, 1u);
    mTarget = mComplete == 0 ? 1 : 0;
    allocateSet(mSets[mTarget]);
    allocateContext();
    // the constants the finished set will be used with: Source ebruneton model.cc:668-686
    const bool precompute_illuminance = mNumWavelengths > 3;
    F64 sky_k_r, sky_k_g, sky_k_b, sun_k_r, sun_k_g, sun_k_b;
    if (precompute_illuminance)
    {
        sky_k_r = sky_k_g = sky_k_b = MAX_LUMINOUS_EFFICACY;
    }
    else
    {
        computeSpectralRadianceToLuminanceFactors(mSpectrum.wavelengths, mSpectrum.solar_irradiance, -3 /* lambda_power */, &sky_k_r, &sky_k_g, &sky_k_b);
    }
    computeSpectralRadianceToLuminanceFactors(mSpectrum.wavelengths, mSpectrum.solar_irradiance, 0 /* lambda_power */, &sun_k_r, &sun_k_g, &sun_k_b);
    mBuildSkyK.set((F32)sky_k_r, (F32)sky_k_g, (F32)sky_k_b);
    mBuildSunK.set((F32)sun_k_r, (F32)sun_k_g, (F32)sun_k_b);
    mBuildWhitePoint = solarWhitePoint(mSpectrum.wavelengths, mSpectrum.solar_irradiance);
    // the runtime's atmosphere: model.cc:768-772 (the shader of the API at kLambdaR, kLambdaG, kLambdaB)
    const F64 rgb[3] = { kLambdaR, kLambdaG, kLambdaB };
    mBuildRuntime = paramsAt(mSpectrum, rgb);
    buildSteps();
    mNextStep = 0;
    mBuilding = true;
    return true;
}

void Atmosphere::buildSteps()
{
    mSteps.clear();
    const Set target = mSets[mTarget];
    const GLuint dIrr = mDeltaIrradiance, dRay = mDeltaRayleigh, dMie = mDeltaMie, dDen = mDeltaDensity;
    const GLuint dMulti = dRay;   // PrecomputedTexturesGenerator.ts:198
    const GLuint fbo = mFbo;

    // the textures and luminanceFromRadiance of PrecomputeMaterial.setUniforms (:242-266) and the transmittance
    // (:486-488, :512-514, :535-537, :583-585)
    auto bindCommon = [=](LLGLSLShader& sh, const F32 lfr[9])
    {
        bindNamed(sh, "transmittanceTexture", 8, GL_TEXTURE_2D, target.transmittance);
        bindNamed(sh, "singleRayleighScatteringTexture", 9, GL_TEXTURE_3D, dRay);
        bindNamed(sh, "singleMieScatteringTexture", 10, GL_TEXTURE_3D, dMie);
        bindNamed(sh, "multipleScatteringTexture", 11, GL_TEXTURE_3D, dMulti);
        bindNamed(sh, "scatteringDensityTexture", 12, GL_TEXTURE_3D, dDen);
        bindNamed(sh, "irradianceTexture", 13, GL_TEXTURE_2D, dIrr);
        const GLint l = glGetUniformLocation(sh.mProgramObject, "luminanceFromRadiance");
        // model.cc:349-353 BindMat3 uploads its row-major mat3 with transpose = true
        if (l >= 0) glUniformMatrix3fv(l, 1, GL_TRUE, lfr);
    };
    auto unbindCommon = []()
    {
        unbindUnit(8, GL_TEXTURE_2D);
        for (S32 u = 9; u <= 12; ++u) unbindUnit(u, GL_TEXTURE_3D);
        unbindUnit(13, GL_TEXTURE_2D);
    };
    // a 2D pass into `tex`
    auto pass2d = [=, this](Pass pass, GLuint tex, S32 w, S32 h, bool additive, Params params, std::array<F32, 9> lfr, S32 order)
    {
        return [=, this]()
        {
            PassScope scope;
            LLGLSLShader& sh = mPrograms[pass];
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            glViewport(0, 0, w, h);
            sh.bind();
            setParams(sh.mProgramObject, params);
            bindCommon(sh, lfr.data());
            const GLint lo = glGetUniformLocation(sh.mProgramObject, "scatteringOrder");
            if (lo >= 0) glUniform1i(lo, order);
            drawScreen(additive);
            unbindCommon();
            sh.unbind();
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        };
    };
    // layers [first, last) of a 3D pass into `tex` (render3DRenderTarget :434-443: uniform `layer`, one draw each)
    auto pass3d = [=, this](Pass pass, GLuint tex, S32 first, S32 last, bool additive, Params params, std::array<F32, 9> lfr, S32 order)
    {
        return [=, this]()
        {
            PassScope scope;
            LLGLSLShader& sh = mPrograms[pass];
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glViewport(0, 0, SCATTERING_TEXTURE_WIDTH, SCATTERING_TEXTURE_HEIGHT);
            sh.bind();
            setParams(sh.mProgramObject, params);
            bindCommon(sh, lfr.data());
            const GLint lo = glGetUniformLocation(sh.mProgramObject, "scatteringOrder");
            if (lo >= 0) glUniform1i(lo, order);
            const GLint ll = glGetUniformLocation(sh.mProgramObject, "layer");
            for (S32 layer = first; layer < last; ++layer)
            {
                glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, 0, layer);
                glDrawBuffer(GL_COLOR_ATTACHMENT0);
                if (ll >= 0) glUniform1i(ll, layer);
                drawScreen(additive);
            }
            unbindCommon();
            sh.unbind();
            glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0, 0, 0);
        };
    };
    const S32 CHUNK = 8;   // layers per step
    auto add3d = [&](Pass pass, GLuint tex, bool additive, const Params& params, const std::array<F32, 9>& lfr, S32 order)
    {
        for (S32 first = 0; first < SCATTERING_TEXTURE_DEPTH; first += CHUNK)
        {
            mSteps.push_back(pass3d(pass, tex, first, llmin(first + CHUNK, SCATTERING_TEXTURE_DEPTH), additive, params, lfr, order));
        }
    };

    // [WolfViewer] OpenGL leaves a new texture's contents undefined (WebGL, which takram targets, zero-fills it):
    // the higher-order table is only ever ADDED to (:705-711), so it starts cleared, as WebGL would give it.
    mSteps.push_back([=]()
    {
        PassScope scope;
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        const F32 zero[4] = { 0.f, 0.f, 0.f, 0.f };
        for (S32 layer = 0; layer < SCATTERING_TEXTURE_DEPTH; ++layer)
        {
            glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target.higher, 0, layer);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            glClearBufferfv(GL_COLOR, 0, zero);
        }
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0, 0, 0);
    });

    // Source: ebruneton atmosphere/model.cc:907-949 Init: the batches of 3 wavelengths and their
    // luminance_from_radiance matrices (row-major, model.h:312 mat3)
    struct Batch { F64 lambdas[3]; std::array<F32, 9> lfr; };
    std::vector<Batch> batches;
    if (mNumWavelengths <= 3)
    {
        batches.push_back({ { kLambdaR, kLambdaG, kLambdaB }, { 1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f } });
    }
    else
    {
        const F64 kMin = 360.0, kMax = 830.0;
        const S32 num_iterations = (S32)((mNumWavelengths + 2) / 3);
        const F64 dlambda = (kMax - kMin) / (3 * num_iterations);
        for (S32 i = 0; i < num_iterations; ++i)
        {
            Batch b;
            b.lambdas[0] = kMin + (3 * i + 0.5) * dlambda;
            b.lambdas[1] = kMin + (3 * i + 1.5) * dlambda;
            b.lambdas[2] = kMin + (3 * i + 2.5) * dlambda;
            // model.cc:925-938 coeff(): without MAX_LUMINOUS_EFFICACY (it comes back in SKY k = 683)
            auto coeff = [dlambda](F64 lambda, S32 component)
            {
                const F64 x = cieColorMatchingFunctionTableValue(lambda, 1);
                const F64 y = cieColorMatchingFunctionTableValue(lambda, 2);
                const F64 z = cieColorMatchingFunctionTableValue(lambda, 3);
                return (F32)((XYZ_TO_SRGB[component * 3] * x + XYZ_TO_SRGB[component * 3 + 1] * y + XYZ_TO_SRGB[component * 3 + 2] * z) * dlambda);
            };
            b.lfr = { coeff(b.lambdas[0], 0), coeff(b.lambdas[1], 0), coeff(b.lambdas[2], 0),
                      coeff(b.lambdas[0], 1), coeff(b.lambdas[1], 1), coeff(b.lambdas[2], 1),
                      coeff(b.lambdas[0], 2), coeff(b.lambdas[1], 2), coeff(b.lambdas[2], 2) };
            batches.push_back(b);
        }
    }

    for (size_t i = 0; i < batches.size(); ++i)
    {
        const Batch& b = batches[i];
        const Params params = paramsAt(mSpectrum, b.lambdas);
        const bool additive = i > 0;   // model.cc:947 blend = i > 0
        // Source: three-geospatial PrecomputedTexturesGenerator.ts:592-717 precompute(context, additive)
        // :597-600 the transmittance
        mSteps.push_back(pass2d(TRANS, target.transmittance, TRANSMITTANCE_TEXTURE_WIDTH, TRANSMITTANCE_TEXTURE_HEIGHT, false, params, b.lfr, 0));
        // :611-626 direct irradiance into deltaIrradiance, and zeros (or nothing, additive) into irradiance
        mSteps.push_back(pass2d(DIRECT_DELTA, dIrr, IRRADIANCE_TEXTURE_WIDTH, IRRADIANCE_TEXTURE_HEIGHT, false, params, b.lfr, 0));
        mSteps.push_back(pass2d(DIRECT_IRR, target.irradiance, IRRADIANCE_TEXTURE_WIDTH, IRRADIANCE_TEXTURE_HEIGHT, additive, params, b.lfr, 0));
        // :635-652 single scattering into deltaRayleigh, deltaMie and (stored or accumulated) scattering
        add3d(SINGLE_RAY, dRay, false, params, b.lfr, 0);
        add3d(SINGLE_MIE, dMie, false, params, b.lfr, 0);
        add3d(SINGLE_SCAT, target.scattering, additive, params, b.lfr, 0);
        // :665-716 the 2nd, 3rd and 4th orders
        for (S32 order = 2; order <= 4; ++order)
        {
            add3d(DENSITY, dDen, false, params, b.lfr, order);                       // :668-672 scatteringOrder = order
            mSteps.push_back(pass2d(INDIRECT_DELTA, dIrr, IRRADIANCE_TEXTURE_WIDTH, IRRADIANCE_TEXTURE_HEIGHT, false, params, b.lfr, order - 1));   // :676-682, :558
            mSteps.push_back(pass2d(INDIRECT_IRR, target.irradiance, IRRADIANCE_TEXTURE_WIDTH, IRRADIANCE_TEXTURE_HEIGHT, true, params, b.lfr, order - 1));   // :683-689
            add3d(MULTI_DELTA, dMulti, false, params, b.lfr, 0);                     // :693-698
            add3d(MULTI_SCAT, target.scattering, true, params, b.lfr, 0);            // :699-704
            add3d(MULTI_SCAT, target.higher, true, params, b.lfr, 0);                // :705-711
        }
    }
    if (mNumWavelengths > 3)
    {
        // Source: ebruneton atmosphere/model.cc:951-963: the transmittance again, at kLambdaR, kLambdaG, kLambdaB
        const std::array<F32, 9> id = { 1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f };
        mSteps.push_back(pass2d(TRANS, target.transmittance, TRANSMITTANCE_TEXTURE_WIDTH, TRANSMITTANCE_TEXTURE_HEIGHT, false, mBuildRuntime, id, 0));
    }
}

bool Atmosphere::precomputeStep()
{
    if (!mBuilding) return true;
    // the screen triangle goes with the pipeline's buffers (LLPipeline::destroyGL): the build waits for it
    if (!mShadersLoaded || !gPipeline.mScreenTriangleVB) return false;
    if (mNextStep < mSteps.size())
    {
        mSteps[mNextStep++]();
        if (mNextStep < mSteps.size()) return false;
    }
    // finished: read back what the CPU needs (PrecomputedTexturesGenerator.ts:742-752), then swap the sets in
    readBack();
    mComplete = mTarget;
    mRuntime = mBuildRuntime;
    mSunK = mBuildSunK;
    mSkyK = mBuildSkyK;
    mWhitePoint = mBuildWhitePoint;
    mBuilding = false;
    mSteps.clear();
    freeContext();   // :740
    return true;
}

void Atmosphere::readBack()
{
    const Set& s = mSets[mTarget];
    mTransmittanceData.assign((size_t)TRANSMITTANCE_TEXTURE_WIDTH * TRANSMITTANCE_TEXTURE_HEIGHT * 4, 0.f);
    mIrradianceData.assign((size_t)IRRADIANCE_TEXTURE_WIDTH * IRRADIANCE_TEXTURE_HEIGHT * 4, 0.f);
    gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, s.transmittance);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, mTransmittanceData.data());
    gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, s.irradiance);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, mIrradianceData.data());
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
}

// [WolfViewer 2026-10-25] the WolfEepGains uniform block (brunetonRuntime.glsl): three std140 arrays of EEP_GAINS vec4 -
// wolfEepGain, wolfEepCloudGain, wolfEepProbeDir - in that order. The gains are written when they change; the probe
// directions only by setEepProbeDirs. Bound to LLGLSLShader::UB_WOLF_EEP_GAINS, the binding point every program's block was
// given at link (llglslshader.cpp mapUniforms).
namespace
{
    constexpr GLsizeiptr EEP_ARRAY_BYTES = Atmosphere::EEP_GAINS * 4 * sizeof(F32);
    constexpr GLsizeiptr EEP_BLOCK_BYTES = 3 * EEP_ARRAY_BYTES;
}

void Atmosphere::bindEepBlock() const
{
    if (!mEepBlock)
    {
        glGenBuffers(1, &mEepBlock);
        glBindBuffer(GL_UNIFORM_BUFFER, mEepBlock);
        std::vector<F32> zero(EEP_BLOCK_BYTES / sizeof(F32), 0.f);
        glBufferData(GL_UNIFORM_BUFFER, EEP_BLOCK_BYTES, zero.data(), GL_DYNAMIC_DRAW);
        mEepBlockDirty = true;
    }
    if (mEepBlockDirty)
    {
        F32 data[2 * EEP_GAINS * 4];
        for (S32 i = 0; i < EEP_GAINS; ++i)
        {
            for (S32 c = 0; c < 3; ++c)
            {
                data[i * 4 + c] = mEepGain[i].mV[c];
                data[(EEP_GAINS + i) * 4 + c] = mEepCloudGain[i].mV[c];
            }
            data[i * 4 + 3] = 1.f;
            data[(EEP_GAINS + i) * 4 + 3] = 1.f;
        }
        glBindBuffer(GL_UNIFORM_BUFFER, mEepBlock);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, 2 * EEP_ARRAY_BYTES, data);
        mEepBlockDirty = false;
    }
    glBindBufferBase(GL_UNIFORM_BUFFER, LLGLSLShader::UB_WOLF_EEP_GAINS, mEepBlock);
}

void Atmosphere::setEepProbeDirs(const LLVector3 dirs[EEP_GAINS])
{
    bindEepBlock();   // creates it, writes the gains if they changed, binds it
    F32 data[EEP_GAINS * 4];
    for (S32 i = 0; i < EEP_GAINS; ++i)
    {
        data[i * 4 + 0] = dirs[i].mV[0]; data[i * 4 + 1] = dirs[i].mV[1]; data[i * 4 + 2] = dirs[i].mV[2]; data[i * 4 + 3] = 0.f;
    }
    glBindBuffer(GL_UNIFORM_BUFFER, mEepBlock);
    glBufferSubData(GL_UNIFORM_BUFFER, 2 * EEP_ARRAY_BYTES, EEP_ARRAY_BYTES, data);
}

void Atmosphere::setRuntimeUniforms(LLGLSLShader& sh, F32 pre_exposure, bool uniforms) const
{
    if (mComplete < 0) return;
    const Set& s = mSets[mComplete];
    if (uniforms)
    {
    setParams(sh.mProgramObject, mRuntime);
    // [WolfViewer] Bruneton's absolute k (model.cc:668-686) x the pre-exposure (Filament "pre-exposed lighting",
    // Filament.md.html:1723), in place of takram's relative k (AtmosphereParameters.ts:182-191)
    const GLint ls = location(sh.mProgramObject, "SUN_SPECTRAL_RADIANCE_TO_LUMINANCE");
    if (ls >= 0) glUniform3f(ls, mSunK.mV[0] * pre_exposure, mSunK.mV[1] * pre_exposure, mSunK.mV[2] * pre_exposure);
    const GLint lk = location(sh.mProgramObject, "SKY_SPECTRAL_RADIANCE_TO_LUMINANCE");
    if (lk >= 0) glUniform3f(lk, mSkyK.mV[0] * pre_exposure, mSkyK.mV[1] * pre_exposure, mSkyK.mV[2] * pre_exposure);
    // [WolfViewer 2026-10-13] the EEP's colours (setEepColours); a program that never gets them reads 0: untouched
    const GLint lo = location(sh.mProgramObject, "wolfEepGainOn");
    if (lo >= 0) glUniform1f(lo, mEepGainOn ? 1.f : 0.f);
    const GLint lco = location(sh.mProgramObject, "wolfEepCloudGainOn");
    if (lco >= 0) glUniform1f(lco, mEepCloudGainOn ? 1.f : 0.f);
    // [WolfViewer 2026-10-25] the azimuths' reference: the stock sky's light (wolfenvironmentrender.cpp eepLightWorld)
    const GLint lgr = location(sh.mProgramObject, "wolfEepGainRef");
    if (lgr >= 0) glUniform3fv(lgr, 1, mEepGainRef.mV);
    const GLint lcgr = location(sh.mProgramObject, "wolfEepCloudGainRef");
    if (lcgr >= 0) glUniform3fv(lcgr, 1, mEepCloudGainRef.mV);
    const GLint lc = location(sh.mProgramObject, "wolfEepCloudTint");
    if (lc >= 0) glUniform3f(lc, mEepCloud.mV[0] - 1.f, mEepCloud.mV[1] - 1.f, mEepCloud.mV[2] - 1.f);
    const GLint lth = location(sh.mProgramObject, "wolfEepCloudThin");
    if (lth >= 0) glUniform1f(lth, mEepCloudThin);
    const GLint lm = location(sh.mProgramObject, "wolfEepMieScale");
    if (lm >= 0) glUniform1f(lm, mEepMie - 1.f);
    // [WolfViewer 2026-10-25] wolftk/nightLight.glsl (WolfEnvironmentRender: the night's light as the eye sees it)
    const GLint ln = location(sh.mProgramObject, "wolfNight");
    if (ln >= 0) glUniform1f(ln, mNight);
    const GLint lns = location(sh.mProgramObject, "wolfNightS");
    if (lns >= 0) glUniform1f(lns, mNightS);
    }
    bindEepBlock();   // [WolfViewer 2026-10-25] the gain grids' uniform block, for every program this is given to
    // AtmosphereMaterialBase.ts:140-144 the tables (single Mie: none, combined)
    bindReserved(sh, LLShaderMgr::TRANSMITTANCE_TEX, GL_TEXTURE_2D, s.transmittance);
    bindReserved(sh, LLShaderMgr::SCATTER_TEX, GL_TEXTURE_3D, s.scattering);
    bindReserved(sh, LLShaderMgr::ILLUMINANCE_TEX, GL_TEXTURE_2D, s.irradiance);
    bindReserved(sh, LLShaderMgr::WOLF_TK_HIGHER_ORDER_TEX, GL_TEXTURE_3D, s.higher);
}

GLuint Atmosphere::transmittanceTexture() const { return mComplete >= 0 ? mSets[mComplete].transmittance : 0; }
GLuint Atmosphere::scatteringTexture() const { return mComplete >= 0 ? mSets[mComplete].scattering : 0; }
GLuint Atmosphere::irradianceTexture() const { return mComplete >= 0 ? mSets[mComplete].irradiance : 0; }
GLuint Atmosphere::higherOrderScatteringTexture() const { return mComplete >= 0 ? mSets[mComplete].higher : 0; }

// Source: three-geospatial packages/atmosphere/src/getSunLightColor.ts:48-103 getSunLightColor (correctAltitude off:
// WolfViewer's planet is a sphere, getAltitudeCorrectionOffset.ts:7-21 gives 0), x Bruneton's absolute SUN k
LLColor3 Atmosphere::sunIlluminance(const LLVector3& pos_km, const LLVector3& sun_dir) const
{
    if (mComplete < 0) return LLColor3::black;
    const Params& a = mRuntime;
    LLVector3 transmittance;
    F64 r = pos_km.length();
    F64 rmu = (F64)pos_km.mV[0] * sun_dir.mV[0] + (F64)pos_km.mV[1] * sun_dir.mV[1] + (F64)pos_km.mV[2] * sun_dir.mV[2];
    const F64 topRadius = a.top_radius;
    const F64 distanceToTop = -rmu - sqrt(rmu * rmu - r * r + topRadius * topRadius);
    if (distanceToTop > 0.0)
    {
        r = topRadius;
        rmu += distanceToTop;
    }
    if (r > topRadius)
    {
        transmittance.set(1.f, 1.f, 1.f);
    }
    else
    {
        const F64 mu = rmu / r;
        if (rayIntersectsGround(a, r, mu))
        {
            transmittance.setZero();
        }
        else
        {
            F64 u, v;
            getUvFromRMu(a, r, mu, u, v);
            transmittance = sampleTexture(mTransmittanceData, TRANSMITTANCE_TEXTURE_WIDTH, TRANSMITTANCE_TEXTURE_HEIGHT, u, v);
        }
    }
    const LLVector3 solar((F32)a.solar_irradiance[0], (F32)a.solar_irradiance[1], (F32)a.solar_irradiance[2]);
    const LLColor3 radiance = mul(transmittance, solar);
    return LLColor3(radiance.mV[0] * mSunK.mV[0], radiance.mV[1] * mSunK.mV[1], radiance.mV[2] * mSunK.mV[2]);
}

// Source: three-geospatial packages/atmosphere/src/SkyLightProbe.ts:85-122 update (correctAltitude off), the
// irradiance x Bruneton's absolute SKY k
LLColor3 Atmosphere::skyIrradiance(const LLVector3& pos_km, const LLVector3& sun_dir) const
{
    if (mComplete < 0) return LLColor3::black;
    const F64 r = pos_km.length();
    const F64 muS = ((F64)pos_km.mV[0] * sun_dir.mV[0] + (F64)pos_km.mV[1] * sun_dir.mV[1] + (F64)pos_km.mV[2] * sun_dir.mV[2]) / r;
    F64 u, v;
    getUvFromRMuS(mRuntime, r, muS, u, v);
    const LLVector3 irradiance = sampleTexture(mIrradianceData, IRRADIANCE_TEXTURE_WIDTH, IRRADIANCE_TEXTURE_HEIGHT, u, v);
    return mul(irradiance, mSkyK);
}

// Source: three-geospatial packages/atmosphere/src/SkyLightProbe.ts:124-129 (and the constants :37-41: L0 = 1 /
// sqrt(pi), L1 = sqrt(3) / (2 sqrt(pi)), three.js SphericalHarmonics3 order 0, y, z, x)
void Atmosphere::skySH(const LLVector3& pos_km, const LLVector3& sun_dir, const LLVector3& normal_world, LLColor3 coeffs[4]) const
{
    const F32 L0_COEFF = 1.f / sqrtf(F_PI);
    const F32 L1_COEFF = sqrtf(3.f) / (2.f * sqrtf(F_PI));
    const LLColor3 irradiance = skyIrradiance(pos_km, sun_dir);
    coeffs[0] = irradiance * L0_COEFF;
    coeffs[1] = irradiance * (L1_COEFF * normal_world.mV[VY]);
    coeffs[2] = irradiance * (L1_COEFF * normal_world.mV[VZ]);
    coeffs[3] = irradiance * (L1_COEFF * normal_world.mV[VX]);
}

// Source: three-geospatial packages/atmosphere/src/shaders/bruneton/runtime.glsl:373-391 GetSunAndSkyIrradiance with
// normal = point / r, and :436-444 GetSunAndSkyIlluminance (the k's)
LLColor3 Atmosphere::horizontalIlluminance(const LLVector3& pos_km, const LLVector3& sun_dir, LLColor3* sun_part, LLColor3* sky_part) const
{
    LLColor3 sun = LLColor3::black, sky = LLColor3::black;
    if (mComplete >= 0)
    {
        const Params& a = mRuntime;
        const F64 r = pos_km.length();
        const F64 mu_s = ((F64)pos_km.mV[0] * sun_dir.mV[0] + (F64)pos_km.mV[1] * sun_dir.mV[1] + (F64)pos_km.mV[2] * sun_dir.mV[2]) / r;
        LLVector3 normal = pos_km;
        normal.normalize();
        // :383-384 sky_irradiance = GetIrradiance(r, mu_s) x (1 + dot(normal, point) / r) x 0.5 (= 1 for the up normal)
        const F64 tilt = (1.0 + (normal * pos_km) / r) * 0.5;
        const LLVector3 irr = getIrradiance(a, mIrradianceData, r, mu_s) * (F32)tilt;
        sky = mul(irr, mSkyK);
        // :387-390 solar_irradiance x GetTransmittanceToSun(r, mu_s) x max(dot(normal, sun_direction), 0)
        const LLVector3 solar((F32)a.solar_irradiance[0], (F32)a.solar_irradiance[1], (F32)a.solar_irradiance[2]);
        const LLVector3 t = transmittanceToSun(a, mTransmittanceData, r, mu_s);
        const F32 cosine = llmax(normal * sun_dir, 0.f);
        sun = mul(LLVector3(solar.mV[0] * t.mV[0], solar.mV[1] * t.mV[1], solar.mV[2] * t.mV[2]) * cosine, mSunK);
    }
    if (sun_part) *sun_part = sun;
    if (sky_part) *sky_part = sky;
    return sun + sky;
}

void Atmosphere::release()
{
    freeSet(mSets[0]);
    freeSet(mSets[1]);
    freeContext();
    mComplete = -1;
    mBuilding = false;
    mSteps.clear();
    mTransmittanceData.clear();
    mIrradianceData.clear();
    mLocations.clear();
    if (mEepBlock) { glDeleteBuffers(1, &mEepBlock); mEepBlock = 0; }
    mEepBlockDirty = true;
}

}   // namespace WolfTk
