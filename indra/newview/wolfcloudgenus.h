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
#include <map>
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

    /** [WolfViewer 2026-10-26] The upper wind for the high layer (cirrus), which MET Locationforecast does not give: its only
     *  wind is "wind_speed ... 10 m above ground" and "wind_from_direction" (https://docs.api.met.no/doc/locationforecast/datamodel,
     *  read 2026-10-26: no pressure-level wind, no shear). Derived from the 10 m wind by the friction layer's turning, the
     *  free-atmosphere (geostrophic) wind above it being the nearest estimate the forecast allows:
     *    Source: https://en.wikipedia.org/wiki/Wind_gradient - the cross-isobar angle "ranges from 10° over open water, to 30° over
     *            rough hilly terrain"; surface friction gives "a reduction of 40% to 50% of the geostrophic wind speed aloft" over
     *            rough terrain, "only 20% to 30%" over open water (the middles taken: 45 % and 25 %).
     *    Source: FAA Aviation Weather (AC 00-6, "Friction", https://www.aviationweather.ws/020_Friction.php) - "The angle of
     *            surface wind to isobars is about 10° over water increasing with roughness of terrain."
     *    Source: https://easaatplexamguide.wordpress.com/2018/05/24/winds/ - the surface wind backs in the northern hemisphere and
     *            veers in the southern relative to the wind aloft (so the wind aloft is VEERED, clockwise, from the 10 m wind in
     *            the north, and backed in the south).
     *  Not derivable from the forecast: the thermal-wind turning and the jet's speed above the boundary layer (they need the
     *  temperature gradient aloft; region-terrain-work/wolf-environment-render/CLOUD_SHAPE_PLAN.md section 3). */
    constexpr F32 UPPER_VEER_SEA_DEG = 10.f, UPPER_VEER_LAND_DEG = 30.f;
    constexpr F32 SURFACE_OVER_UPPER_SEA = 1.f - 0.25f, SURFACE_OVER_UPPER_LAND = 1.f - 0.45f;

    /** [WolfViewer 2026-10-26] How strongly each stretched lookup's flow field bends it (cloudsCommon.glsl wolfFlowWarp: x the
     *  modes' own bend angles). The cirrus in full; cloud streets a third: horizontal convective rolls are "parallel to the mean
     *  wind" of the boundary layer (BEAUTY_RESEARCH.md 9.3, https://en.wikipedia.org/wiki/Horizontal_convective_rolls), so they
     *  only follow its slow synoptic curvature. A modelling number (CLOUD_SHAPE_PLAN.md section 5.1). */
    constexpr F32 FLOW_BEND_UPPER = 1.f, FLOW_BEND_STREETS = 1.f / 3.f;

    /** [WolfViewer 2026-10-26, third pass] The cirrus layer's own horizontal scale: one tile of the weather texture's high channel
     *  (cloudsCommon.glsl, the kind-2 lookup's wolfUpperRepeat = (R pi / 2) / this), independent of the EEP's cloud_scale, which
     *  sizes the low and middle layers (wolfenvironmentrender.cpp s_repeat). With the EEP's 0.28 a tile was ~357 km and one cirrus
     *  band ~240 x 30 km (Paul's ICELAND "comet" and the straight bands). Measured cirrus: generating cells and fall streaks on a
     *  ~1 km scale, organised "mesoscale uncinus complexes" up to ~120-150 km across (Sassen et al., FIRE Cirrus lidar,
     *  https://ntrs.nasa.gov/api/citations/19910001165/downloads/19910001165.pdf); fall streaks up to ~60 km long from a parent
     *  cloud ~20 km wide (Ann. Geophys. 20:711, 2002, https://angeo.copernicus.org/articles/20/711/2002/). A tile of 120 km (the
     *  complex's size) gives the high channel's Perlin (frequency 6 x 12 per tile, localWeatherF.glsl) base periods of 80 km along
     *  the wind after the 4x stretch and 10 km across: streaks ~40 km long and ~5 km wide at the largest octave, its finer octaves
     *  and the fibres (wolftkclouds.cpp FIBRE_PERIOD_*, ~1 km filaments) below that - inside the measured ranges. */
    constexpr F32 CIRRUS_TILE_M = 120000.f;

    /** [WolfViewer 2026-10-26] The low and middle layers' own horizontal scale (WolfCloudPhysicalScale, on by default; off = the EEP's
     *  cloud_scale as before): one tile of the weather texture's low / middle channels. Paul 10-26 (TROMSO, Wolf Territories Home):
     *  "cardboard cut-out" clouds and "cartoon" puffs. With the EEP's cloud_scale (repeat 0.28 - 0.38 x takram's 100) a tile was
     *  262 - 357 km, so the low channel's Worley cells (frequency 16, 32, 64, 128 per tile, localWeatherF.glsl "Low clouds") were
     *  16 - 22 km across down to 2 - 2.8 km, and the middle channel's (8 .. 64) 33 - 45 km: each cloud a smooth weather-cell outline
     *  tens of km wide, far larger than the 3D shape noise that should give it its billows (wolftkclouds.cpp shapeRepeat 0.0003 / m:
     *  one 128^3 texture per 3.3 km). Measured cumulus: the cloud size distribution is a power law up to a scale break of
     *  400 - 1,250 m, and clouds of about that size carry most of the cloud fraction (Neggers, Jonker & Siebesma 2003, "Size
     *  statistics of cumulus cloud populations in large-eddy simulations", J. Atmos. Sci. 60:1060-1074, Table 2: BOMEX 700 m,
     *  SCMS 1,050 m, ARM 400 - 1,250 m). A 32 km tile makes the low channel's cells 2 km down to 250 m, so the clouds the coverage cut
     *  leaves are about 1 km across (part of a cell) - at that scale break - and the shape and detail noise shape each one; the
     *  middle channel's cells are 4 km down to 500 m. Cloud streets (4x along the wind) become rolls 2 km apart, the low end of
     *  the 2 - 6 x boundary-layer depth of horizontal convective rolls (BEAUTY_RESEARCH.md 9.3). The tile is a modelling number
     *  chosen from those sizes, not a measurement. */
    constexpr F32 LOW_MID_TILE_M = 32000.f;

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
        F32  latitude = 45.f;        // [2026-10-26] degrees, the place's (WolfRegionWeather autoEnvLat): the hemisphere of the upper wind's turn
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
        F32  fibre = 0.f;                       // [2026-10-26] 0..1 the fibrous (hair-like) detail of an ice layer (cloudsCommon.glsl wolfLayerFibre)
        F32  repeat = 1.f;                      // [2026-10-26] the genus's own weather repeat (GenusRow::repeat; WolfTk::CloudLayer::repeat)
    };

    struct Result
    {
        Layer layer[4];              // takram channels r (low), g (middle), b (high), a (fog / pannus slab)
        F32   rotationDeg = 0.f;     // the weather texture turned to the wind (cloud streets, cirrus filaments)
        F32   stretch = 1.f;         // and stretched along it ..
        // .. [WolfViewer 2026-10-14] for these layers only (1 = stretched, 0 = turned only): the cirrus layer, and the low
        // layer when it forms streets. The stretch used to apply to every layer, so one thin Ci layer drew the cumulus and
        // altocumulus below it as long streaks too (Paul: "all the clouds ... seem to be long ... wherever i go").
        // [WolfViewer 2026-10-26] three lookups: 0 = turned only, 1 = stretched along the 10 m wind (cloud streets), 2 = stretched
        // along the upper wind (cirrus, upperRotationDeg) - the streets and the cirrus no longer share one direction.
        F32   layerStretch[4] = { 0.f, 0.f, 0.f, 0.f };
        F32   upperRotationDeg = 0.f;   // [2026-10-26] the cirrus lookup turned to the upper wind (UPPER_VEER_*), as rotationDeg
        F32   upperWindFrom = 240.f, upperWindSpeed = 0.f;   // [2026-10-26] that wind (degrees from, m/s): the cirrus layer's drift
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

    /** [WolfViewer 2026-10-26] THE CLOUD TABLE. Paul: "no more whole-viewer recompiles per sky tweak". Every tunable number of
     *  the cloud renderer that lived in C++ - the genus rows, the rules' bases and tops, the tiles, the stretch, the curved-flow
     *  bend, the genus easing time, the build-clearance fade, the fibre periods, the upper-wind rule - is read from
     *  app_settings/wolf_clouds.xml (LLSD XML, the citations carried as XML comments) at start and again whenever the shaders
     *  are rebuilt (LLViewerShaderMgr::setShaders -> WolfEnvironmentRender::loadShaders; llviewermenu.cpp reload_vertex_shader),
     *  so a sky change is: edit the file or a .glsl, reload, look. The constexpr values above are the compiled defaults: the
     *  table starts from them, a file that leaves a value out keeps them, and a file that does not parse or fails validation
     *  is logged (LL_WARNS, the reason) and the previous table kept. One INFO line per load names the path and the counts.
     *  Not in the file (compile-time): the shaders' own constants (cloudsCommon.glsl WOLF_FLOW_* modes, the ice phase define
     *  WOLF_ICE_G_SCALE and WOLF_MS_ATTENUATION from ICE_G / MS_ATTENUATION above, the takram quality presets' step counts) -
     *  those are GLSL consts and #defines, re-read with the shaders on the same reload. */
    struct GenusRow
    {
        F32 shape = 1.f, detail = 1.f, exponent = 1.f, bias = 0.35f, filter = 0.6f;
        WolfTk::DensityProfile profile = { 0.f, 0.f, 0.75f, 0.25f };
        F32 lwc = 0.3f, ice = 0.f, absorption = 0.f;
        bool shadow = true;
        F32 fibre = 0.f;
        F32 repeat = 1.f;   // the weather texture's repeat x this for the genus (whole number): its cells' size (cloudsCommon.glsl wolfLayerRepeat)
        bool streaks = false;   // [2026-10-26] read through the upper-wind streak lookup (stretched, bent, fibre-carved): Ci; a veil (Cs) is not
    };
    /** A rule's heights: < 0 = the rule's own compiled value. `depth` (top = base + depth) is for the rules whose base is the
     *  lifted condensation level (cumulus): it wins over `top` when both are given. */
    struct HeightRule { F32 base = -1.f, top = -1.f, depth = -1.f; };
    struct Table
    {
        std::map<std::string, GenusRow> genera;
        std::map<std::string, HeightRule> heights;
        F32 lowMidTileM = LOW_MID_TILE_M, cirrusTileM = CIRRUS_TILE_M;
        F32 stretch = 4.f;                                   // the streets' and the cirrus' elongation along the wind (map())
        F32 flowBendStreets = FLOW_BEND_STREETS, flowBendUpper = FLOW_BEND_UPPER;
        F32 genusTauS = 20.f;                                // the genus easing time (wolfenvironmentrender.cpp GENUS_TAU)
        F32 buildClearM0 = 20.f, buildClearW = 50.f;         // WolfCloudClearance M0 / W
        F32 fibrePeriodAcrossM = 2000.f, fibrePeriodAlongM = 20000.f, fibrePeriodVerticalM = 4000.f;   // wolftkclouds.cpp FIBRE_PERIOD_*
        F32 upperVeerSeaDeg = UPPER_VEER_SEA_DEG, upperVeerLandDeg = UPPER_VEER_LAND_DEG;
        F32 surfaceOverUpperSea = SURFACE_OVER_UPPER_SEA, surfaceOverUpperLand = SURFACE_OVER_UPPER_LAND;
        F32 towerShareMediocris = 1.f - 0.25f, towerShareCongestus = 1.f - 0.6f;   // towerShareOf
        F32 cuBaseMinM = 200.f, cuBaseMaxM = 2400.f;         // rule 4: the cumulus base held between these
        bool coveragePerLayer = true;                        // each layer's coverage from its own MET fraction (wolfenvironmentrender.cpp)
        std::string path;
        U32 generation = 0;                                  // 0 = the compiled defaults, +1 per successful load
    };
    const Table& table();
    /** Reads app_settings/wolf_clouds.xml into the table (see above). True when the file was taken. */
    bool loadTable();
    U32 tableGeneration();
}

#endif // WOLF_CLOUD_GENUS_H
