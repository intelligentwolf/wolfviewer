/**
 * @file wolfexposure.cpp
 * @brief WolfViewer: Filament's physically based camera exposure. See wolfexposure.h.
 *
 * Translated for WolfViewer (IntelligentWolf Ltd, 2026-10-12) from Google Filament, commit
 * 764fe6cac9bdb4ba24d3d852344dfa04a5f70f75, filament/src/Exposure.cpp.
 *
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "linden_common.h"

#include "wolfexposure.h"

#include <algorithm>
#include <cmath>

namespace WolfExposure
{

// Source: Exposure.cpp:31-47 ev100(aperture, shutterSpeed, sensitivity)
F32 ev100(F32 const aperture, F32 const shutterSpeed, F32 const sensitivity)
{
    // With N = aperture, t = shutter speed and S = sensitivity,
    // we can compute EV100 knowing that:
    //
    // EVs = log2(N^2 / t)
    // and
    // EVs = EV100 + log2(S / 100)
    //
    // We can therefore find:
    //
    // EV100 = EVs - log2(S / 100)
    // EV100 = log2(N^2 / t) - log2(S / 100)
    // EV100 = log2((N^2 / t) * (100 / S))
    //
    // Reference: https://en.wikipedia.org/wiki/Exposure_value
    return std::log2((aperture * aperture) / shutterSpeed * 100.0f / sensitivity);
}

// Source: Exposure.cpp:49-62 ev100FromLuminance
F32 ev100FromLuminance(F32 const luminance)
{
    // With L the average scene luminance, S the sensitivity and K the
    // reflected-light meter calibration constant:
    //
    // EV = log2(L * S / K)
    //
    // With the usual value K = 12.5 (to match standard camera manufacturers
    // settings), computing the EV100 becomes:
    //
    // EV100 = log2(L * 100 / 12.5)
    //
    // Reference: https://en.wikipedia.org/wiki/Exposure_value
    return std::log2(luminance * (100.0f / 12.5f));
}

// Source: Exposure.cpp:64-79 ev100FromIlluminance
F32 ev100FromIlluminance(F32 const illuminance)
{
    // With E the illuminance, S the sensitivity and C the incident-light meter
    // calibration constant, the exposure value can be computed as such:
    //
    // EV = log2(E * S / C)
    // or
    // EV100 = log2(E * 100 / C)
    //
    // Using C = 250 (a typical value for a flat sensor), the relationship between
    // EV100 and illuminance is:
    //
    // EV100 = log2(E * 100 / 250)
    //
    // Reference: https://en.wikipedia.org/wiki/Exposure_value
    return std::log2(illuminance * (100.0f / 250.0f));
}

// Source: Exposure.cpp:86-91 exposure(aperture, shutterSpeed, sensitivity)
F32 exposure(F32 const aperture, F32 const shutterSpeed, F32 const sensitivity)
{
    // This is equivalent to calling exposure(ev100(N, t, S))
    // By merging the two calls we can remove extra pow()/log2() calls
    const F32 e = (aperture * aperture) / shutterSpeed * 100.0f / sensitivity;
    return 1.0f / (1.2f * e);
}

// Source: Exposure.cpp:93-136 exposure(ev100)
F32 exposure(F32 const ev100)
{
    // The photometric exposure H is defined by:
    //
    // H = (q * t / (N^2)) * L
    //
    // Where t is the shutter speed, N the aperture, L the incident luminance
    // and q the lens and vignetting attenuation. A typical value of q is 0.65
    // (see reference link below).
    //
    // The value of H as recorded by a sensor depends on the sensitivity of the
    // sensor. An easy way to find that value is to use the saturation-based
    // sensitivity method:
    //
    // S_sat = 78 / H_sat
    //
    // This method defines the maximum possible exposure that does not lead to
    // clipping or blooming.
    //
    // The factor 78 is chosen so that exposure settings based on a standard
    // light meter and an 18% reflective surface will result in an image with
    // a grey level of 18% * sqrt(2) = 12.7% of saturation. The sqrt(2) factor
    // is used to account for an extra half a stop of headroom to deal with
    // specular reflections.
    //
    // Using the definitions of H and S_sat, we can derive the formula to
    // compute the maximum luminance to saturate the sensor:
    //
    // H_sat = 78 / S_stat
    // (q * t / (N^2)) * Lmax = 78 / S
    // Lmax = (78 / S) * (N^2 / (q * t))
    // Lmax = (78 / (S * q)) * (N^2 / t)
    //
    // With q = 0.65, S = 100 and EVs = log2(N^2 / t) (in this case EVs = EV100):
    //
    // Lmax = (78 / (100 * 0.65)) * 2^EV100
    // Lmax = 1.2 * 2^EV100
    //
    // The value of a pixel in the fragment shader can be computed by
    // normalizing the incident luminance L at the pixel's position
    // with the maximum luminance Lmax
    //
    // Reference: https://en.wikipedia.org/wiki/Film_speed
    return 1.0f / (1.2f * std::pow(2.0f, ev100));
}

// Source: Exposure.cpp:143-148 luminance(aperture, shutterSpeed, sensitivity)
F32 luminance(F32 const aperture, F32 const shutterSpeed, F32 const sensitivity)
{
    // This is equivalent to calling luminance(ev100(N, t, S))
    // By merging the two calls we can remove extra pow()/log2() calls
    const F32 e = (aperture * aperture) / shutterSpeed * 100.0f / sensitivity;
    return e * 0.125f;
}

// Source: Exposure.cpp:150-168 luminance(ev100)
F32 luminance(F32 const ev100)
{
    // With L the average scene luminance, S the sensitivity and K the
    // reflected-light meter calibration constant:
    //
    // EV = log2(L * S / K)
    // L = 2^EV100 * K / 100
    //
    // As in ev100FromLuminance(luminance), we use K = 12.5 to match common camera
    // manufacturers (Canon, Nikon and Sekonic):
    //
    // L = 2^EV100 * 12.5 / 100 = 2^EV100 * 0.125
    //
    // With log2(0.125) = -3 we have:
    //
    // L = 2^(EV100 - 3)
    //
    // Reference: https://en.wikipedia.org/wiki/Exposure_value
    return std::pow(2.0f, ev100 - 3.0f);
}

// Source: Exposure.cpp:175-180 illuminance(aperture, shutterSpeed, sensitivity)
F32 illuminance(F32 const aperture, F32 const shutterSpeed, F32 const sensitivity)
{
    // This is equivalent to calling illuminance(ev100(N, t, S))
    // By merging the two calls we can remove extra pow()/log2() calls
    const F32 e = (aperture * aperture) / shutterSpeed * 100.0f / sensitivity;
    return 2.5f * e;
}

// Source: Exposure.cpp:182-199 illuminance(ev100)
F32 illuminance(F32 const ev100)
{
    // EV100 = log2(E * 100 / 250)
    // E = 2^EV100 / (100 / 250)
    // E = 2.5 * 2^EV100
    //
    // Reference: https://en.wikipedia.org/wiki/Exposure_value
    return 2.5f * std::pow(2.0f, ev100);
}

// Source: Krawczyk, Myszkowski & Seidel 2005 eq. 11 (Reinhard 2002 section 3 "key"): alpha = 1.03 - 2 / (2 + log10(L + 1))
F32 keyKrawczyk(F32 const log_avg_cdm2)
{
    return 1.03f - 2.0f / (2.0f + std::log10(std::max(log_avg_cdm2, 0.f) + 1.0f));
}

// Source: wolftk/filamentAgx.glsl wolfAgxToneMapper on a grey (Filament ToneMapper.cpp:711-730 agxDefaultContrastApprox,
// :763-788): log2 encoding over [AgxMinEv, AgxMaxEv], the 7th-order sigmoid, then the 2.2 linearisation
F32 agxGreyDisplay(F32 const exposed_grey)
{
    const F32 minEv = -12.47393f, maxEv = 4.026069f;
    F32 x = (std::log2(std::max(exposed_grey, 1e-10f)) - minEv) / (maxEv - minEv);
    x = llclamp(x, 0.f, 1.f);
    const F32 x2 = x * x, x4 = x2 * x2, x6 = x4 * x2;
    const F32 s = -17.86f * x6 * x + 78.01f * x6 - 126.7f * x4 * x + 92.06f * x4 - 28.72f * x2 * x + 4.361f * x2 - 0.1718f * x + 0.002857f;
    return std::pow(std::max(s, 0.f), 2.2f);
}

F32 agxGreyExposed(F32 const display_grey)
{
    // the curve is monotone over the encoded range: bisection in log2 space between its ends
    F32 lo = -12.47393f, hi = 4.026069f;
    for (S32 i = 0; i < 40; ++i)
    {
        const F32 mid = 0.5f * (lo + hi);
        if (agxGreyDisplay(std::exp2(mid)) < display_grey) lo = mid; else hi = mid;
    }
    return std::exp2(0.5f * (lo + hi));
}

// [WOLF ENV TAKRAM 2026-10-12] WolfViewer's meter: see wolfexposure.h for every step and its source.
// [WolfViewer round 9] see wolfexposure.h nightFloorEv100: Table 2's full-moon row from a quarter moon's light up, its
// quarter-moon / medium-aurora rows with no moon, linear in EV between
F32 Meter::nightFloorEv100(F32 moon_direct_lux)
{
    if (!std::isfinite(moon_direct_lux)) moon_direct_lux = 0.f;
    // [WolfViewer 2026-10-13] round 9's floor again: LL lights, full-bright and emissive no longer scale with the exposure
    // (physicalLightColor, llsettingsvo.cpp WOLF_FULLBRIGHT), so the night can be exposed to be seen.
    const F32 t = llclamp(moon_direct_lux / QUARTER_MOON_LUX, 0.f, 1.f);
    return MOONLESS_FLOOR_EV100 + (NIGHT_FLOOR_EV100 - MOONLESS_FLOOR_EV100) * t;
}

F32 Meter::update(F32 illuminance_lux, F32 dt, F32 compensation_ev, F32 frame_log_avg_cdm2, F32 sky_luminance_share, F32 moon_direct_lux)
{
    // a NaN or infinite reading (std::max passes a NaN through) is taken as darkness, never into the exposure
    if (std::isnan(illuminance_lux)) illuminance_lux = 0.f;
    else if (std::isinf(illuminance_lux)) illuminance_lux = illuminance_lux > 0.f ? 1.0e9f : 0.f;   // +inf: very bright, not dark
    if (!std::isfinite(compensation_ev)) compensation_ev = 0.f;
    if (!std::isfinite(frame_log_avg_cdm2)) frame_log_avg_cdm2 = -1.f;   // no reading
    if (!std::isfinite(sky_luminance_share)) sky_luminance_share = 1.f;
    sky_luminance_share = llclamp(sky_luminance_share, 0.f, 1.f);
    // incident metering (Exposure.cpp:64-79), never below the night's floor for the moon there is (Wikipedia Table 2,
    // ANSI PH2.7; nightFloorEv100, round 9)
    const F32 E = std::max(illuminance_lux, 1.0e-12f);
    const F32 floorEv = nightFloorEv100(moon_direct_lux) - EC_18_PERCENT;
    const F32 evIncident = ev100FromIlluminance(E) - EC_18_PERCENT;
    if (frame_log_avg_cdm2 > 0.f)
    {
        // the frame's own reading (wolfexposure.h: reflected, the key on the display side, 18 %): the exposed
        // log-average a day scene gets is 2^(EV_r - 3) / (1.2 2^EV_r) x 1.728 = 0.18 (K 12.5 + 18 %); the display
        // AgX makes of it is scaled by the key ratio and taken back through the curve
        const F32 dayDisplay = agxGreyDisplay(0.18f);
        const F32 keyRatio = keyKrawczyk(frame_log_avg_cdm2) / keyKrawczyk(DAY_KEY_CDM2);
        const F32 exposedAvg = agxGreyExposed(dayDisplay * keyRatio);          // what the log-average is to be, exposed
        const F32 exposureR = exposedAvg / frame_log_avg_cdm2;                  // the pre-exposure for that
        const F32 evR = std::log2(1.f / (1.2f * exposureR));                   // Exposure.cpp:93-136 inverted
        // the key is the adapting luminance's (Krawczyk 2005: a property of the scene's log-average, not of what
        // fills the frame), so the diffuse bound carries the same shift the reflected target got from it: a moonlit
        // field filling the frame stays a night scene, a sunlit beach stays at its albedo
        const F32 keyShift = evR - (ev100FromLuminance(frame_log_avg_cdm2) - EC_18_PERCENT);
        const F32 evIncidentKeyed = evIncident + keyShift;
        // the diffuse bound (wolfexposure.h): the geometry's share never darker than the incident meter's
        F32 evTarget = sky_luminance_share * evR + (1.f - sky_luminance_share) * std::min(evR, evIncidentKeyed);
        // [WolfViewer round 13] the bound holds only for light the incident meter sees: a passive Lambertian surface under an
        // illuminance E has at most the radiance of a white one, E / pi (Lambert). A frame whose log-average is brighter than
        // that is lit by light the meter does not read - the aurora in the reflection probes that light the scene's
        // ambient, lamps at night - and the bound then forced a night beach under a bright aurora up to the night floor's
        // exposure (Paul, 2026-10-13 06:01Z: meter 0.00054 lux, EV100 -6.79 = the floor, the sand lit like day). Above
        // E / pi the reflected reading is trusted, blended over one stop so nothing pops.
        const F32 whiteL = E / F_PI;
        if (whiteL > 0.f)
        {
            const F32 t = llclamp(std::log2(frame_log_avg_cdm2 / whiteL), 0.f, 1.f);
            evTarget = evTarget + (evR - evTarget) * t;
        }
        // the night floor is the incident meter's (a darker night is exposed as a moonlit one): it does not lift a frame the
        // reflected reading says is lit by light the incident meter misses (above)
        mTargetEv100 = std::max(evTarget, floorEv);
    }
    else
    {
        mTargetEv100 = std::max(evIncident, floorEv);
    }
    // the luminance that EV denotes (Exposure.cpp:150-168), the quantity Filament's adaptation smooths
    const F32 L = luminance(mTargetEv100);
    if (!mValid || !std::isfinite(mLavg) || mLavg <= 0.f)
    {
        mLavg = L;
        mValid = true;
    }
    else
    {
        // Krawczyk PhD 2007 eq. 4.7 sigma(Y) = 0.04 / (0.04 + Y) and eq. 4.12 tau(Y) = sigma(Y) tau_rods + (1 - sigma(Y))
        // tau_cones, with Y the frame's actual adapting luminance (section 4.2.2: Y, not the filtered Y_a) - here the
        // luminance the meter reads now
        const F32 sigma = 0.04f / (0.04f + L);
        const F32 tau = sigma * TAU_RODS + (1.f - sigma) * TAU_CONES;
        // Filament.md.html "Adaptation": L_avg = L_avg + (L - L_avg) (1 - e^(-dt tau)), tau there a rate:
        // with Krawczyk's time constant (eq. 4.5, e^(-T / tau)) the rate is 1 / tau
        const F32 step = (dt > 0.f) ? dt : 0.f;   // a NaN or negative step adapts nothing
        mLavg = mLavg + (L - mLavg) * (1.f - std::exp(-step / tau));
    }
    mEv100 = ev100FromLuminance(mLavg);
    // Filament.md.html "Exposure compensation": EV100' = EV100 - EC
    mExposure = WolfExposure::exposure(mEv100 - compensation_ev);
    // the look's contrast pivot: the frame's middle as it is exposed (Hable's logMid); middle grey without a reading
    mTonePivot = (frame_log_avg_cdm2 > 0.f) ? llclamp(frame_log_avg_cdm2 * mExposure, 1e-5f, 10.f) : 0.18f;
    return mExposure;
}

} // namespace WolfExposure
