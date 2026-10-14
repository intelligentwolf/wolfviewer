/**
 * @file wolfcloudgenus.h
 * @brief The weather -> cloud genera mapping for the native Wolf sky: MET's three cloud fractions, fog, precipitation,
 *        thunder, wind, temperature and humidity -> four cloud layers, each a WMO genus with its base, top, coverage,
 *        extinction, phase and shape (the takram cloud layer tuple).
 *
 * WolfViewer (IntelligentWolf Ltd, 2026-10-13), step 3 of the native-sky programme. Sources, all in
 * region-terrain-work/wolf-environment-render/BEAUTY_RESEARCH.md:
 *   section 9.4   the 26 rules (first match per band): MET Locationforecast fields, metno weather_symbol Factory.cpp,
 *                 WMO cloud atlas definitions, BUFR 0 20 012 cloud types, NWS fog guidance, Espy's LCL
 *   section 8.1   per-genus base / thickness / LWC / phase (WMO atlas, ISCCP, Wood / Stephens extinction)
 *   section 8.4   the renderer tuple per genus
 *   section 9.3   cloud streets along the wind, the diurnal cumulus rule
 * The harness twin is scratchpad phys2/genus.py (genus_views.py scores it against reference/clouds/); the two tables
 * must stay in step.
 */

#ifndef WOLF_CLOUD_GENUS_H
#define WOLF_CLOUD_GENUS_H

#include "stdtypes.h"
#include "llmath.h"   // F_PI
#include "wolftkclouds.h"
#include <string>
#include <vector>

namespace WolfCloudGenus
{
    /** Magnus formula, Alduchov & Eskridge 1996 (J. Appl. Meteor. 35:601-609): a = 17.625, b = 243.04 C;
     *  gamma = ln(RH/100) + a T / (b + T); Td = b gamma / (a - gamma). T in C, RH in %. */
    F32 dewPoint(F32 T_c, F32 RH_pct);
    /** Cumulus base: 125 m x (T - Td) (Espy, the lifted condensation level; section 9.3 - cumuliform only). */
    F32 lclMetres(F32 T_c, F32 Td_c);

    /** The ice phase: the asymmetry parameter of roughened ice crystals over water droplets'. van Diedenhoven et al.
     *  2013 (Atmos. Chem. Phys. 13:3185-3203, abstract): "Median asymmetry parameters retrieved by the RSP range from
     *  0.76 to 0.78, and are generally smaller than those currently assumed in most climate models and satellite
     *  retrievals ... the measurements indicate roughened or distorted ice crystals"; water droplet clouds g = 0.85
     *  (Stephens, AT622 notes section 15, example 15.5). The cloud shader's WOLF_ICE_G_SCALE scales its forward lobe. */
    constexpr F32 ICE_G = 0.76f, WATER_G = 0.85f;
    constexpr F32 ICE_G_SCALE = ICE_G / WATER_G;

    /** The multiple-scattering octaves' attenuation (takram clouds.frag approximateMultipleScattering, Hillaire 2016's
     *  a = b = c with "a <= b"; takram 0.5). Set from a cloud's albedo: the saturated octave sum reproduces rho at 90 deg,
     *  sum a^k = (rho / pi) / phase(90 deg): rho 0.8 (BEAUTY_RESEARCH.md 3.3, Mitchell & Wallace 1992 via PSU: thick cloud
     *  albedo 0.6-0.9), the two-lobe HG (0.7 / -0.2, mix 0.5) at 90 deg 0.0471 sr^-1 -> a = 1 - 0.0471 pi / 0.8 = 0.815.
     *  With takram's 0.5 a sunlit top came out at a third of the photographs' (section 10 A2). */
    constexpr F32 CLOUD_ALBEDO = 0.8f, HG_PHASE_90 = 0.0471f;
    constexpr F32 MS_ATTENUATION = 1.f - HG_PHASE_90 * F_PI / CLOUD_ALBEDO;

    /** What the forecast gives (auto_env_lib.php:346-384, wolfenvironmentstate.cpp fromEntry). Fractions 0..1. */
    struct Weather
    {
        F32  low = 0.f, medium = 0.f, high = 0.f, total = 0.f;
        F32  fog = 0.f;              // fog_area_fraction
        F32  precip = 0.f;           // mm next 1 h
        F32  thunderPct = 0.f;       // probability_of_thunder %
        std::string symbol;          // symbol_code
        F32  wind = 3.f, windFrom = 240.f;   // m/s, degrees from
        F32  T = 15.f, RH = 60.f;    // C, %
        bool haveTemperature = false;
        F32  hour = 12.f;            // local solar hour
        bool sunUp = true;
        F32  highTrend = 0.f;        // d(high fraction) / dt over the next hours, per hour; 0 unknown
        bool land = true;
        bool summer = true;
        // [WolfViewer 2026-10-14] the region's EEP cloud share (WolfEepReference::cloudCoveredFraction), carried by the low band:
        // the diurnal rule fades the forecast's own cumulus but never below this, because the EEP says these clouds are there
        F32  eepLow = 0.f;
    };

    /** One layer of the result: the takram tuple plus the genus fields the shader got in step 3. */
    struct Layer
    {
        std::string genus = "none";  // WMO abbreviation or none
        char channel = 'r';
        F32  altitude = 0.f, height = 0.f;      // metres above the sea; height 0 = no layer
        F32  densityScale = 0.f;                // sigma / scatteringCoefficient (1), 1/m
        F32  shapeAmount = 1.f, shapeDetailAmount = 1.f, weatherExponent = 1.f, shapeAlteringBias = 0.35f, coverageFilterWidth = 0.6f;
        WolfTk::DensityProfile profile = { 0.f, 0.f, 0.75f, 0.25f };
        F32  coverage = 0.f;                    // the band's fraction
        F32  ice = 0.f;                         // 0 water .. 1 ice
        F32  absorption = 0.f;                  // per unit density (precipitation darkening)
        bool shadow = false;
        F32  tower = 0.f;                       // [2026-10-14] how far the column tops spread (towerShareOf; cloudsCommon.glsl)
    };

    struct Result
    {
        Layer layer[4];              // takram channels r (low), g (middle), b (high), a (fog / pannus slab)
        F32   rotationDeg = 0.f;     // the weather texture turned to the wind (cloud streets, cirrus filaments)
        F32   stretch = 1.f;         // and stretched along it ..
        // .. [WolfViewer 2026-10-14] for these layers only (1 = stretched, 0 = turned only): the cirrus layer, and the low
        // layer when it forms streets. The stretch used to apply to every layer, so one thin Ci layer drew the cumulus and
        // altocumulus below it as long streaks too (Paul: "all the clouds ... seem to be long ... wherever i go").
        F32   layerStretch[4] = { 0.f, 0.f, 0.f, 0.f };
        F32   dewPoint = 0.f, lcl = 0.f;
        std::vector<std::string> notes;   // the rules that fired (the UI's "inferred" list, section 9.5)
        /** The one coverage the haze modulation and the ground irradiance keep using: the largest layer's. */
        F32 maxCoverage() const;
    };

    /** Section 9.4. */
    Result map(const Weather& w);

    /** The takram layers for a result (WolfTk::Clouds::layers()); a smoothed result's values go straight in. */
    void toCloudLayers(const Result& r, WolfTk::CloudLayers& out);

    /** Nothing pops (section 10 A23, Nubis 2017 transitions over 10-30 s): every number of `cur` moves towards
     *  `target` with time constant `tau` seconds (exponential approach, dt seconds); the genus names and channels
     *  jump (they only label), the numbers never do. A layer appearing from nothing grows from the target's base
     *  with zero height and coverage. */
    void approach(Result& cur, const Result& target, F32 dt, F32 tau);

    /** [WolfViewer 2026-10-14] the lowest column top of a convective genus as a share of its layer's depth (cloudsCommon.glsl
     *  wolfLayerTower), 0 = a flat-topped band; and the same for a layer (0 while it has no body). */
    F32 towerShareOf(const std::string& genus);
    F32 towerShare(const Layer& l);
}

#endif // WOLF_CLOUD_GENUS_H
