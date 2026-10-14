/**
 * @file wolfexposure.h
 * @brief WolfViewer: Filament's physically based camera exposure, for the native Wolf sky.
 *
 * Translated for WolfViewer (IntelligentWolf Ltd, 2026-10-12) from Google Filament, commit
 * 764fe6cac9bdb4ba24d3d852344dfa04a5f70f75, filament/include/filament/Exposure.h and
 * filament/src/Exposure.cpp, and docs_src/src_markdeep/Filament.md.html "Physically based camera".
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
 *
 * Changes from the source: the Camera overloads (which read FCamera's aperture / shutter / ISO) are
 * replaced by explicit-argument calls; WolfExposure::Meter (below) is WolfViewer's use of these functions.
 */

#ifndef WOLF_EXPOSURE_H
#define WOLF_EXPOSURE_H

#include "stdtypes.h"
#include "llmath.h"   // F_PI

namespace WolfExposure
{
    // Source: Exposure.cpp:31-47 ev100(aperture, shutterSpeed, sensitivity)
    F32 ev100(F32 aperture, F32 shutterSpeed, F32 sensitivity);
    // Source: Exposure.cpp:49-62 ev100FromLuminance (K = 12.5)
    F32 ev100FromLuminance(F32 luminance);
    // Source: Exposure.cpp:64-79 ev100FromIlluminance (C = 250, flat sensor)
    F32 ev100FromIlluminance(F32 illuminance);
    // Source: Exposure.cpp:86-91 exposure(aperture, shutterSpeed, sensitivity)
    F32 exposure(F32 aperture, F32 shutterSpeed, F32 sensitivity);
    // Source: Exposure.cpp:93-136 exposure(ev100) = 1 / (1.2 * 2^EV100)
    F32 exposure(F32 ev100);
    // Source: Exposure.cpp:143-148 luminance(aperture, shutterSpeed, sensitivity)
    F32 luminance(F32 aperture, F32 shutterSpeed, F32 sensitivity);
    // Source: Exposure.cpp:150-168 luminance(ev100) = 2^(EV100 - 3)
    F32 luminance(F32 ev100);
    // Source: Exposure.cpp:175-180 illuminance(aperture, shutterSpeed, sensitivity)
    F32 illuminance(F32 aperture, F32 shutterSpeed, F32 sensitivity);
    // Source: Exposure.cpp:182-199 illuminance(ev100) = 2.5 * 2^EV100
    F32 illuminance(F32 ev100);

    /**
     * [WOLF ENV TAKRAM 2026-10-12] The native sky's camera: an INCIDENT light meter at the camera (the
     * illuminance on a horizontal receptor from the atmosphere's sun and sky, the clouds' shadow on it), so
     * what the camera looks at - sky, sand, a wall - never changes the exposure (Paul's decision D1).
     *   EV100   = ev100FromIlluminance(E)                         (Exposure.cpp:64-79)
     *   floor   EV100 >= -3: full moonlight, moon above 40 deg, "-3 to -2" (Wikipedia "Exposure value",
     *           Table 2, from ANSI PH2.7-1973 / PH2.7-1986 exposure guides; revision 1352562239,
     *           2026-05-04) - a darker night is exposed as a moonlit one, so it looks dark, not like a
     *           long photograph
     *   adaptation  L_avg += (L - L_avg) (1 - e^(-dt / tau))     (Filament.md.html "Adaptation", eq.
     *           adaptation, after Pattanaik et al. 2000), on the luminance the EV denotes
     *           L = luminance(EV100) (Exposure.cpp:150-168); Filament's text leaves tau open, so the time
     *           constants are Krawczyk, Myszkowski & Seidel's: tau_rods 0.4 s, tau_cones 0.1 s, blended by
     *           the rods' sensitivity sigma(Y) = 0.04 / (0.04 + Y) at the luminance metered now (Krawczyk PhD 2007
     *           eq. 4.5-4.7, 4.12)
     *   compensation  EV100' = EV100 - EC                          (Filament.md.html "Exposure compensation"),
     *           EC = log2(RenderExposure) - the viewer's exposure slider keeps its meaning (1 = none)
     *   exposure = exposure(EV100')                               (Exposure.cpp:93-136)
     */
    class Meter
    {
    public:
        /** A new start: the next update() takes the target at once (wolfenvironmentrender.cpp: the native sky coming
         *  back on, a region change - teleport or crossing - and a GL rebuild). */
        void reset() { mValid = false; }
        /** One frame. `illuminance_lux` the incident illuminance at the camera, `dt` seconds since the last
         *  call, `compensation_ev` EC, `frame_log_avg_cdm2` the previous frame's log-average luminance (Reinhard
         *  2002 eq. 1, wolftk/logLumF.glsl; <= 0 = none: the incident reading alone). Returns the pre-exposure
         *  for this frame.
         *
         *  The target EV100 (2026-10-13, round 6; BEAUTY_RESEARCH.md section 4.3 / 4.5 item 5):
         *    reflected  EV_r = ev100FromLuminance(L_w)                   (Exposure.cpp:49-62, K 12.5) - the frame's own
         *               log-average luminance, as Unreal's "Average Scene EV100" and Narkowicz's histogram mean
         *    key        EV_r - log2(alpha(L_w) / alpha_day), alpha(L) = 1.03 - 2 / (2 + log10(L + 1)) (Krawczyk,
         *               Myszkowski & Seidel 2005 "Perceptual effects in real-time tone mapping", eq. 11, after
         *               Reinhard 2002's key): a dim scene is rendered dimmer than a bright one, not lifted to the
         *               same grey; alpha_day the key of a 100 klux day's middle grey (0.18 x 100 000 / pi cd/m2,
         *               Filament.md.html's measured sun / Frostbite's 100 000 lux), so a sunny day is the reference
         *    18 %       - EC_18 = log2(0.18 x 9.6): Filament's 1 / (1.2 x 2^EV100) puts the EV's middle grey
         *               2^(EV100 - 3) at 1 / 9.6 = 10.4 % (room for specular, Filament.md.html "Exposure"); the
         *               photographic convention it names ("exposed for as 18% middle-gray", "Exposure compensation")
         *               is 18 %: + 0.79 EV
         *  Without a frame reading the incident EV100 - EC_18 (first frame, probe faces). Never below the night
         *  floor (in the same 18 % terms). The adaptation and EC follow as before.
         *
         *  Round 6b (2026-10-13):
         *    display-side key  the key is the DISPLAY value of the log-average (Reinhard 2002 section 3 "scaled
         *               luminance", eq. 2: L = a / L_w x L_w(x, y), followed by a curve that is identity in the
         *               shadows). Through Filament AgX the shadows are compressed (the sigmoid's toe), so the exposure
         *               is chosen so that AgX's display of the exposed log-average equals alpha(L_w) / alpha_day x
         *               AgX's display of a day's 0.18 (K 12.5 + 18 %): agxGreyExposed below. A day scene is
         *               unchanged; a moonlit one comes out where the key says, not a further 1.5 stops under it.
         *    diffuse bound  `sky_luminance_share` is the share of the frame's light that is sky (wolftk/logLumF.glsl;
         *               LL's own luminanceF.glsl treats sky and objects apart for the same reason). The geometry's
         *               share is bounded by Filament's incident meter, which exposes a diffuse surface at its albedo
         *               (Filament.md.html "Exposure value and illuminance": a Lambertian scene): its target is never
         *               darker than the incident EV100 - EC_18. The sky's share takes the reflected target as it is
         *               (the sky is a source, not a reflector). Target = share x reflected + (1 - share) x
         *               min(reflected, incident + key shift): a sand-filled frame is exposed at the sand's albedo, a
         *               sunset at its sky. The key shift (the EV the key added to the reflected target) is carried
         *               into the bound: the key belongs to the adapting luminance, not to what fills the frame, so a
         *               moonlit field filling the frame stays a night scene. */
        F32 update(F32 illuminance_lux, F32 dt, F32 compensation_ev, F32 frame_log_avg_cdm2 = -1.f, F32 sky_luminance_share = 1.f,
                   F32 moon_direct_lux = 0.f);
        /** [WolfViewer round 9] the night floor for the light there is: Wikipedia "Exposure value" Table 2 (ANSI PH2.7)
         *  gives separate rows for the night's light sources - "Moonlight, Moon altitude > 40 deg: Full -3 to -2,
         *  Gibbous -4, Quarter -6", "Aurora borealis and australis: Medium -6 to -5" - so the floor follows the moon's
         *  direct horizontal illuminance at the camera: NIGHT_FLOOR_EV100 (-3, the full-moon row, as approved in round
         *  6b) from a quarter moon's light up, MOONLESS_FLOOR_EV100 (-6, the quarter-moon / medium-aurora rows: the
         *  darkest scene rows of the table; its Milky Way row -11 to -9 exposes the Milky Way itself and rendered the
         *  starlit sky a flat 0.25 grey in the harness) with no moon, linear in EV between (the quarter row sits 3 EV
         *  under the full row: 1/8 of the full moon's illuminance). Under that floor Krawczyk's key (eq. 11, its lower
         *  bound 0.03 at the dark end) sets the starlit sky's level - a dark grey, never black. A moonless night was
         *  floored at -3 before and rendered black (Paul, 2026-10-13 03:35Z: 0.00054 lux, the starlit sky alone). The
         *  18 % convention is applied as to the rest. */
        static F32 nightFloorEv100(F32 moon_direct_lux);
        /** [WolfViewer round 9] a cut - an environment preset switched by hand, a sky swapped without a transition:
         *  the next update() takes the target at once instead of adapting through it, so no frame is black or white
         *  on the way (Krawczyk's adaptation is a model of continuous viewing; a cut has no counterpart in it).
         *  wolfenvironmentrender.cpp calls it when the metered light moves more than CUT_EV in one frame, which no
         *  natural change does (the sun's limb crossing the horizon takes minutes for a few EV). */
        void cut() { mValid = false; }
        F32 exposure() const { return mExposure; }
        /** The frame's log-average luminance as exposed this frame (cd/m2 x exposure): the tone look's contrast pivot
         *  (wolftk/filamentAgx.glsl wolf_tone_pivot, Hable's logMid = the image's middle); 0.18 without a reading. */
        F32 tonePivot() const { return mTonePivot; }
        F32 ev100() const { return mEv100; }          // the adapted EV100 before compensation
        F32 targetEv100() const { return mTargetEv100; }
        F32 adaptedLuminance() const { return mLavg; }
    private:
        bool mValid = false;
        F32  mLavg = 0.f;
        F32  mEv100 = 0.f, mTargetEv100 = 0.f;
        F32  mExposure = 1.f;
        F32  mTonePivot = 0.18f;
    };

    /** The tone curve on a grey: Filament AgX (wolftk/filamentAgx.glsl wolfAgxToneMapper, look NONE) takes a grey
     *  exposed value to a display-linear grey - the inset / outset matrices keep grey, so it is the log encoding, the
     *  sigmoid and the 2.2 linearisation alone (ToneMapper.cpp:711-730, :763-788). The constants are the shader's;
     *  the two must stay in step. Used to place the key on the display side (Reinhard 2002 section 3: the key is the
     *  display value the log-average is mapped to), so AgX's toe does not darken a dim scene a second time. */
    F32 agxGreyDisplay(F32 exposed_grey);
    /** The exposed grey that AgX maps to `display_grey` (bisection over the monotone curve). */
    F32 agxGreyExposed(F32 display_grey);

    /** Source: Wikipedia "Exposure value" Table 2 (ANSI PH2.7-1973 / -1986): "Moonlight, Moon altitude > 40 deg,
     *  Full: -3 to -2" - the lower end. */
    constexpr F32 NIGHT_FLOOR_EV100 = -3.f;
    /** Source: the same Table 2: "Moonlight ... Quarter: -6", "Aurora borealis and australis: Medium -6 to -5" - the
     *  darkest scene rows, for a night with no moon (see Meter::nightFloorEv100). */
    constexpr F32 MOONLESS_FLOOR_EV100 = -6.f;
    /** Source: Krisciunas & Schaefer 1991 eq. 8-9 at phase angle 0: m = -12.73, I* = 10^(-0.4 (m + 16.57)) = 0.02911
     *  foot-candles = 0.3134 lux above the atmosphere; Table 2's quarter-moon row is 3 EV under the full-moon row, so a
     *  quarter moon's light is taken as 1/8 of it. */
    constexpr F32 FULL_MOON_TOA_LUX = 0.3134f;
    constexpr F32 QUARTER_MOON_LUX = FULL_MOON_TOA_LUX / 8.f;
    /** A one-frame change in the metered light no natural cause gives (see Meter::cut). */
    constexpr F32 CUT_EV = 4.f;
    /** Source: Krawczyk PhD 2007 eq. 4.6: tau_rods = 0.4 s, tau_cones = 0.1 s. */
    constexpr F32 TAU_RODS = 0.4f, TAU_CONES = 0.1f;
    /** Source: Krawczyk, Myszkowski & Seidel 2005 eq. 11 (Reinhard 2002's key): alpha = 1.03 - 2 / (2 + log10(L + 1)). */
    F32 keyKrawczyk(F32 log_avg_cdm2);
    /** The key of a 100 klux day's middle grey, 0.18 x 100 000 / pi cd/m2 (Filament.md.html measured sun 100k lux). */
    constexpr F32 DAY_KEY_CDM2 = 0.18f * 100000.f / F_PI;
    /** Filament's EV100 middle grey 2^(EV - 3) x 1 / (1.2 x 2^EV) = 1 / 9.6; the 18 % convention is 0.18 x 9.6 = 1.728 x. */
    constexpr F32 EC_18_PERCENT = 0.78903f;   // log2(1.728)
}

#endif // WOLF_EXPOSURE_H
