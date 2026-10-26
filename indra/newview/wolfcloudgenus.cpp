/**
 * @file wolfcloudgenus.cpp
 * @brief The weather -> cloud genera mapping (see wolfcloudgenus.h for the sources). The twin of scratchpad
 *        phys2/genus.py: the rules and the per-genus rows are the same, line for line.
 */

#include "llviewerprecompiledheaders.h"
#include "wolfcloudgenus.h"

#include "lldir.h"            // [2026-10-26] the cloud table: app_settings/wolf_clouds.xml
#include "llfile.h"
#include "llsd.h"
#include "llsdserialize.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace WolfCloudGenus
{
    // Source: Alduchov & Eskridge 1996, Magnus formula constants a = 17.625, b = 243.04 C (genus.py dew_point)
    F32 dewPoint(F32 const T, F32 const RH)
    {
        const F32 a = 17.625f, b = 243.04f;
        const F32 rh = llclamp(RH, 1.f, 100.f);
        const F32 g = std::log(rh / 100.f) + a * T / (b + T);
        return b * g / (a - g);
    }

    // Source: Espy's equation, LCL = 125 m x (T - Td) (BEAUTY_RESEARCH.md section 9.3)
    F32 lclMetres(F32 const T, F32 const Td)
    {
        return std::max(125.f * (T - Td), 0.f);
    }

    namespace
    {
        // sigma = 3 LWC / (2 rho_w r_e) (Stephens 1978; Wood eq. 2, section 8.1): r_e 10 um water, 30 um ice (genus.py)
        F32 sigmaWater(F32 lwc_g_m3) { return 3.f * lwc_g_m3 * 1e-3f / (2.f * 1000.f * 10e-6f); }
        F32 sigmaIce(F32 iwc_g_m3) { return 3.f * iwc_g_m3 * 1e-3f / (2.f * 917.f * 30e-6f); }

        /** The compiled genus rows (genus.py GENERA, section 8.4): style, shape amounts, exponent, bias, filter width,
         *  density profile (exp, exponent, linear, constant), LWC / IWC g/m3, ice, absorption, shadow, fibre, repeat.
         *  [WolfViewer 2026-10-26] These are the DEFAULTS of the cloud table (wolfcloudgenus.h Table): app_settings/wolf_clouds.xml
         *  ships the same values with the same citations and is what the viewer reads; this copy serves until the file is read,
         *  and when the file cannot be. Keep the two in step. */
        std::map<std::string, GenusRow> defaultGenera()
        {
            // GenusRow: shape, detail, exponent, bias, filter, profile {exp, exponent, linear, constant}, lwc g/m3, ice, absorption, shadow, fibre, repeat, streaks
            // streaks [2026-10-26, finding 3 at ICELAND 15:55Z]: only Ci and the anvil's Ci spissatus are drawn as streaks along the upper wind
            // (the stretched, bent, fibre-carved lookup). Cirrostratus is by definition a veil - WMO: "transparent, whitish cloud veil of
            // fibrous (hair-like) or smooth appearance, totally or partly covering the sky" (https://cloudatlas.wmo.int/en/clouds-genera-cirrostratus.html) -
            // so it reads the turned lookup like the water layers: low-frequency variation only, never discrete stretched ribbons with clear
            // sky between (Cs at coverage 1 was drawn as a fan of ribbons). Cirrocumulus "grains, ripples" the same, with its small repeat.
            std::map<std::string, GenusRow> R;
            // tops level: a flat profile, the semi-circle rounding only (WMO humilis). Repeat 2: fragments of 1 km down to 125 m
            // (the low channel's Worley 16..128 cells over the 16 km it then spans) - small humilis are a few hundred metres
            R["Cu_humilis"]   = { 1.0f, 1.0f, 1.0f, 0.25f, 0.6f, { 0.f, 0.f, 0.f, 1.f },      0.3f,   0.f,  0.f,   true, 0.f, 2.f, false };
            // takram's default profile: domes
            R["Cu_mediocris"] = { 1.0f, 1.0f, 1.0f, 0.35f, 0.6f, { 0.f, 0.f, 0.75f, 0.25f },  0.5f,   0.f,  0.f,   true, 0.f, 1.f, false };
            // towers: density growing to the top
            R["Cu_congestus"] = { 1.0f, 1.0f, 1.0f, 0.45f, 0.6f, { 0.f, 0.f, 1.f, 0.1f },     1.0f,   0.f,  0.f,   true, 0.f, 1.f, false };
            // the anvil: an exponential flare towards the top; rain: absorption (Schneider 2015 p. 69)
            R["Cb"]           = { 1.0f, 1.0f, 1.0f, 0.6f,  0.7f, { 0.6f, 2.f, 0.f, 0.4f },    2.0f,   0.f,  0.02f, true, 0.f, 1.f, false };
            // a slab of rolls / tessellations (takram's exponent 1 and filter width 0.6: a sharpened filter cut hard edges, round 8)
            // [WolfViewer 2026-10-26] LWC 0.17 (was 0.45). Paul 10-26 (Wolf Territories Home, TROMSO): flat, cut-out clouds with no
            // shading. With 0.45 g/m3 (sigma 0.0675 /m, the log's dens0) over the 900 m slab the densest column had an optical depth
            // of 61: an opaque wall at any thickness above a few per cent of its density, so the edge was optically hard and the
            // inside saturated to one grey. The satellite cloud classes put stratocumulus in the middle optical-thickness band of
            // the low clouds, 3.6 - 23 (ISCCP: Rossow & Schiffer 1999, Bull. Amer. Meteor. Soc. 80:2261-2287, Fig. 2 - three optical-
            // thickness categories per height level, the low ones named cumulus, stratocumulus, stratus; the band edges 0 - 3.6,
            // 3.6 - 23, 23 - 379 as Marsden 2002 quotes them, arXiv:physics/0201032; cumulus and altocumulus share the
            // thinnest band, arXiv:2312.00308: "Cu are mistakenly identified as Ac due to their cloud optical thickness
            // belonging to the same category"). The densest column is set to the band's top,
            // 23 over 900 m (rules 3, 19, 21, 22): sigma 0.0256 /m = sigmaWater(LWC) -> LWC = 0.0256 x 2 x 1000 x 10e-6 / 3 / 1e-3.
            // Repeat 2: the UiT Tromso webcam 10-26 16:49 local shows separate Sc fragments 2 - 10 deg wide at ~1 km: 50 - 500 m
            R["Sc"]           = { 1.0f, 0.6f, 1.0f, 0.5f,  0.6f, { 0.f, 0.f, 0.f, 1.f },      0.17f,  0.f,  0.f,   true, 0.f, 2.f, false };
            // a uniform thin sheet, no cauliflower
            R["St"]           = { 0.3f, 0.0f, 1.0f, 0.9f,  0.3f, { 0.f, 0.f, 0.f, 1.f },      0.25f,  0.f,  0.f,   true, 0.f, 1.f, false };
            // the ground slab, in the shadow map (round 8): the march's optical depth to the light at a sample is two short
            // steps plus the shadow map's column (clouds.frag:528-545 marchOpticalDepth + sampleShadowOpticalDepth), so a
            // slab left out of the map was lit by the unattenuated beam throughout - the sun's warm colour, not the fog's grey
            R["Fog"]          = { 0.2f, 0.0f, 1.0f, 0.95f, 0.2f, { 0.f, 0.f, 0.f, 1.f },      0.25f,  0.f,  0.f,   true, 0.f, 1.f, false };
            // a deep smooth slab, rain darkening; the sun blotted out by its depth
            R["Ns"]           = { 0.4f, 0.0f, 1.0f, 0.9f,  0.4f, { 0.f, 0.f, 0.f, 1.f },      0.3f,   0.f,  0.02f, true, 0.f, 1.f, false };
            // thin elements with shading (exponent 1.5: separate patches). Repeat 8: WMO Altocumulus, "the regularly arranged small
            // elements usually have an apparent width of between one and five degrees" (https://cloudatlas.wmo.int/en/altocumulus-ac.html);
            // the middle channel's Worley 8..64 cells over the 4 km it then spans are 500 - 62 m, 7 - 1 deg from 3 km at 45 deg up
            R["Ac"]           = { 1.0f, 0.5f, 1.5f, 0.5f,  0.5f, { 0.f, 0.f, 0.f, 1.f },      0.3f,   0.f,  0.f,   true, 0.f, 8.f, false };
            // a uniform sheet, ice at the top (section 8.1: three layers)
            // [WolfViewer 2026-10-26] LWC 0.061 (was 0.3). Paul's TROMSO "cardboard cut-out" (10-26, after 14:33Z: L1 As 3000 m + 2500 m):
            // 0.3 g/m3 (sigma 0.045 /m) over 2,500 m gave the densest column an optical depth of 112, so every As patch was an opaque
            // flat slab with a hard edge. ISCCP's altostratus is the middle optical-thickness band of the middle clouds, 3.6 - 23
            // (Rossow & Schiffer 1999 and Marsden 2002, as Sc above; above 23 is nimbostratus). The densest column is set to 23 over
            // the 2,500 m of rule 13: sigma 0.0092 /m -> LWC = 0.0092 x 2 x 1000 x 10e-6 / 3 / 1e-3 = 0.061 (the 2,000 m of rule 7
            // gives 18). The water formula, as make() uses for ice < 1
            R["As"]           = { 0.3f, 0.0f, 1.0f, 0.9f,  0.3f, { 0.f, 0.f, 0.f, 1.f },      0.061f, 0.4f, 0.f,   true, 0.f, 1.f, false };
            // a thin veil eroded little; the filaments come from the texture stretched along the wind; ice.
            // [WolfViewer 2026-10-26] IWC 0.0068 (was 0.03). Paul at ICELAND 10-26 (09:03Z, sun on the horizon): the cirrus a
            // "burning comet" - with 0.03 g/m3 (sigma 1.64 /km) the 2 km layer had a vertical optical depth up to 3.3 (6-13 on a
            // slant), an opaque slab lit by the unshadowed high sun while everything below lay in the dim dawn light. Measured
            // cirrus is translucent: mean optical depth 0.37 +- 0.18 (lidar, 10 % subvisible / 49 % thin / 41 % opaque on Sassen
            // and Cho's classes, Atmos. Chem. Phys. 13:11853, 2013, https://acp.copernicus.org/articles/13/11853/2013/), so the
            // densest part (weather density 1) is set to twice that mean, tau 0.74 over the 2 km: sigma = 0.74 / 2000 m =
            // 3.7e-4 /m = sigmaIce(IWC) -> IWC = 3.7e-4 x 2 x 917 x 30e-6 / 3 / 1e-3 = 0.0068 g/m3 (inside the 0.0005-1 of 8.1).
            // Fibre 0.8 [10-26 fourth pass]: the fibres now carve the weather value BEFORE the coverage cut (cloudsCommon.glsl
            // sampleWeather), +- this much of the 0..1 range: filaments with gaps between them, not a smooth ribbon (Paul's
            // "spokes" at ICELAND / TROMSO 10-26). WMO Cirrus: "white, delicate filaments ... a fibrous (hair-like) appearance"
            R["Ci"]           = { 0.2f, 0.0f, 1.2f, 0.6f,  0.6f, { 0.f, 0.f, 0.f, 1.f },      0.0068f, 1.f, 0.f,   false, 0.8f, 1.f, true };
            // [WolfViewer 2026-10-26] the anvil's cirrus keeps the dense value: Ci spissatus is "sufficiently dense to appear
            // greyish when viewed towards the Sun" (WMO International Cloud Atlas, https://cloudatlas.wmo.int/en/cirrus-spissatus.html)
            R["Ci_spissatus"] = { 0.2f, 0.0f, 1.2f, 0.6f,  0.6f, { 0.f, 0.f, 0.f, 1.f },      0.03f,  1.f,  0.f,   false, 0.4f, 1.f, true };
            // a transparent veil: IWC 0.008 over 2 km -> tau 0.9 (section 8.4 tau <~ 1). Filter 0.6 [10-26 fourth pass] (was 0.2):
            // at the partial coverage the EEP's share leaves it, 0.2 cut the veil into hard-edged ribbons (ICELAND 14:35Z, Cs
            // shown at 0.38); WMO Cirrostratus "veil of fibrous (hair-like) or smooth appearance" - fibre 0.4
            R["Cs"]           = { 0.2f, 0.0f, 1.0f, 0.9f,  0.6f, { 0.f, 0.f, 0.f, 1.f },      0.008f, 1.f,  0.f,   false, 0.4f, 1.f, false };
            // grains < 1 deg (exponent 2: small separate elements); repeat 16: WMO Cirrocumulus elements "apparent width of less
            // than one degree" - the middle channel's cells over 2 km, 250 - 31 m, under 2 deg from 7.5 km
            R["Cc"]           = { 0.8f, 0.0f, 2.0f, 0.5f,  0.4f, { 0.f, 0.f, 0.f, 1.f },      0.03f,  1.f,  0.f,   false, 0.f, 16.f, false };
            return R;
        }

        /** The compiled heights of the rules (make()'s second and third arguments as they were): wolf_clouds.xml 'heights'. A rule
         *  whose base is the cumulus base (the LCL) carries a depth. */
        std::map<std::string, HeightRule> defaultHeights()
        {
            std::map<std::string, HeightRule> H;
            auto bt = [&](const char* k, F32 b, F32 t) { HeightRule h; h.base = b; h.top = t; H[k] = h; };
            auto dp = [&](const char* k, F32 d) { HeightRule h; h.depth = d; H[k] = h; };
            { HeightRule h; h.top = 12000.f; H["4 Cb"] = h; }   // the base is the cumulus base (LCL), the top 12,000 m above the sea
            bt("4 anvil Ci_spissatus", 9000.f, 11000.f);
            bt("7 Ns", 500.f, 4000.f); bt("7 St pannus", 150.f, 500.f); bt("7 As", 4000.f, 6000.f);
            dp("5 Cb showers", 6000.f); dp("5 Cu_congestus showers", 5000.f);
            bt("6 Sc", 700.f, 1700.f); bt("6 St pannus", 300.f, 600.f);
            dp("4b Cu_congestus", 5000.f);
            bt("8 St drizzle", 300.f, 700.f); bt("8 Sc drizzle", 500.f, 1500.f);
            bt("3 Sc advection", 300.f, 1200.f);
            bt("1 Fog saturated", 0.f, 250.f); bt("1 Fog", 0.f, 120.f);
            bt("2 St nebulosus", 250.f, 500.f);
            dp("17 Cu_humilis", 300.f); dp("17 Cu_mediocris", 1200.f);
            bt("19 Sc", 600.f, 1500.f); bt("20 St", 150.f, 500.f); bt("21 Sc sheet", 600.f, 1500.f);
            bt("13 As", 3000.f, 5500.f); bt("15 Ac castellanus", 3000.f, 4200.f); bt("14 Ac", 3000.f, 3800.f);
            bt("11 Cs", 7000.f, 9000.f); bt("9 Cs invading", 7500.f, 9500.f); bt("9 Ci invading", 7500.f, 9500.f);
            bt("10 Cc", 7500.f, 8200.f); bt("10 Ci", 7500.f, 9500.f);
            return H;
        }

        Table defaultTable()
        {
            Table t;
            t.genera = defaultGenera();
            t.heights = defaultHeights();
            return t;
        }

        Table& mutableTable()
        {
            static Table t = defaultTable();
            return t;
        }

        /** [2026-10-26] A layer from a rule: `rule` is its key in the table's heights (base / top / depth override the compiled
         *  `base` / `top`; a depth is measured from the base, so a cumulus rule keeps its LCL base). */
        Layer make(const char* rule, const std::string& genus, F32 base, F32 top, F32 cover, char channel)
        {
            const Table& t = table();
            const auto git = t.genera.find(genus);
            const GenusRow g = git != t.genera.end() ? git->second : GenusRow();   // loadTable guarantees every genus the rules name
            const auto hit = t.heights.find(rule);
            if (hit != t.heights.end())
            {
                const HeightRule& h = hit->second;
                if (h.base >= 0.f) base = h.base;
                if (h.depth >= 0.f) top = base + h.depth;
                else if (h.top >= 0.f) top = h.top;
            }
            Layer l;
            l.genus = genus; l.channel = channel;
            l.altitude = base; l.height = std::max(top - base, 0.f);
            l.densityScale = g.ice >= 1.f ? sigmaIce(g.lwc) : sigmaWater(g.lwc);
            l.shapeAmount = g.shape; l.shapeDetailAmount = g.detail; l.weatherExponent = g.exponent; l.shapeAlteringBias = g.bias; l.coverageFilterWidth = g.filter;
            l.profile = g.profile;
            l.coverage = llclamp(cover, 0.f, 1.f);
            l.ice = g.ice; l.absorption = g.absorption; l.shadow = g.shadow;
            l.tower = towerShareOf(genus);   // [2026-10-14] cloudsCommon.glsl wolfLayerTower
            l.fibre = g.fibre;               // [2026-10-26] cloudsCommon.glsl wolfLayerFibre
            l.repeat = g.repeat;             // [2026-10-26] cloudsCommon.glsl wolfLayerRepeat
            return l;
        }
        Layer none(char channel) { Layer l; l.channel = channel; return l; }

        bool showers(const std::string& s) { return s.find("showers") != std::string::npos; }
        // WolfAutoEnvironmentModule.cs:294 (the symbol says thunder, or >= 50 % chance)
        bool isThunder(const std::string& s, F32 pct) { return s.find("thunder") != std::string::npos || pct >= 50.f; }
    }

    F32 Result::maxCoverage() const
    {
        F32 c = 0.f;
        for (const Layer& l : layer) if (l.height > 0.f) c = std::max(c, l.coverage);
        return c;
    }

    // Source: BEAUTY_RESEARCH.md section 9.4, first match per band (genus.py map_weather)
    Result map(const Weather& w)
    {
        Result r;
        const F32 Td = w.haveTemperature ? dewPoint(w.T, w.RH) : w.T - 5.f;   // no temperature: a 5 K spread (LCL 625 m, "usually below 550 m" .. 2400 m)
        const F32 lcl = lclMetres(w.T, Td);
        r.dewPoint = Td; r.lcl = lcl;
        const Table& tab = table();
        F32 cuBase = llclamp(std::max(lcl, tab.cuBaseMinM), tab.cuBaseMinM, tab.cuBaseMaxM);   // rule 4: max(125 (T - Td), 200 m); Cu up to 2400 m
        Layer& R = r.layer[0]; Layer& G = r.layer[1]; Layer& B = r.layer[2]; Layer& A = r.layer[3];
        R = none('r'); G = none('g'); B = none('b'); A = none('a');
        const bool day = w.sunUp;
        const bool thunder = isThunder(w.symbol, w.thunderPct) && w.precip > 0.f;   // Factory.cpp isThunder = thunder && precipitation > 0
        const bool foggy = w.fog >= 0.5f && w.precip == 0.f;                          // Factory.cpp isFog = fog && precipitation == 0 (the threshold is not in the source; 50 %)
        const bool steady = w.precip > 0.f && w.total > 0.86f && !showers(w.symbol);  // rules 7 / 23: steady precipitation under > 86 % (7 oktas)
        std::string genus;
        // ---- the low band (r) and the fog / stratus slab (a) ----
        if (thunder)
        {
            R = make("4 Cb", "Cb", cuBase, 12000.f, std::max(w.low, 0.3f), 'r'); r.notes.push_back("4 Cb capillatus incus");
            B = make("4 anvil Ci_spissatus", "Ci_spissatus", 9000.f, 11000.f, std::max(w.high, 0.4f), 'b'); r.notes.push_back("4 anvil Ci spissatus");
        }
        else if (steady)
        {
            R = make("7 Ns", "Ns", 500.f, 4000.f, 1.f, 'r'); r.notes.push_back("7 Ns + pannus");
            A = make("7 St pannus", "St", 150.f, 500.f, std::min(1.f, 0.5f + 0.5f * w.low), 'a'); r.notes.push_back("7 pannus (St fractus) below");
            G = make("7 As", "As", 4000.f, 6000.f, std::max(w.medium, 0.8f), 'g'); r.notes.push_back("7 As above");
        }
        else if (w.precip > 0.f && showers(w.symbol))
        {
            if (day && w.summer)
            {
                const bool heavy = w.precip >= 0.95f;   // Factory.cpp precipitation limits {0.1, 0.25, 0.95} mm: heavy
                // [WolfViewer 2026-10-14] congestus "+3-5 km" deep (BEAUTY_RESEARCH.md 8.4; was +3 km): with its tower share
                // (towerShareOf, 0.6) the columns' tops spread over 3 to 5 km above the base instead of one flat 3 km slab
                R = make(heavy ? "5 Cb showers" : "5 Cu_congestus showers", heavy ? "Cb" : "Cu_congestus", cuBase, cuBase + (heavy ? 6000.f : 5000.f), std::max(w.low, 0.3f), 'r');
                r.notes.push_back("5 Cu congestus / Cb calvus showers");
            }
            else
            {
                R = make("6 Sc", "Sc", 700.f, 1700.f, std::max(w.low, 0.5f), 'r');
                A = make("6 St pannus", "St", 300.f, 600.f, 0.3f * w.low, 'a');
                r.notes.push_back("6 Sc with pannus (night / winter showers)");
            }
        }
        else if (w.thunderPct >= 20.f && w.low > 0.f)
        {
            // [WolfViewer 2026-10-14] audit B1: towering cumulus before the rain. Every congestus and Cb path above needs rain in
            // the next hour, so a dry hour with thunder forecast (MET Locationforecast next_1_hours probability_of_thunder) drew
            // fair-weather cumulus. Thunder needs the cumulus to grow first: "All cumulus clouds develop because of convection
            // ... can grow in height and potentially transform into cumulonimbus" (Met Office low-level clouds), congestus being
            // "markedly sprouting and often of great vertical extent" (WMO International Cloud Atlas, Cumulus congestus). 20 %
            // is the threshold this table already reads as "thunder later" (rule 15, Ac castellanus, BEAUTY_RESEARCH.md 9.4).
            R = make("4b Cu_congestus", "Cu_congestus", cuBase, cuBase + 5000.f, w.low, 'r');
            r.notes.push_back("4b Cu congestus (thunder forecast, no rain yet)");
        }
        else if (w.precip > 0.f && w.low >= 0.86f && w.medium < 0.3f)
        {
            const bool st = w.wind < 3.f;
            R = make(st ? "8 St drizzle" : "8 Sc drizzle", st ? "St" : "Sc", st ? 300.f : 500.f, st ? 700.f : 1500.f, w.low, 'r'); r.notes.push_back("8 St / Sc drizzle");
        }
        else if (foggy)
        {
            if (w.wind > 7.7f && !w.land)
            {
                R = make("3 Sc advection", "Sc", 300.f, 1200.f, std::max(w.fog, w.low), 'r'); r.notes.push_back("3 advection fog lifted to Sc (wind > 7.7 m/s)");
            }
            else
            {
                // full cover (round 8b): BEAUTY_RESEARCH.md 9.4 rule 1 - the sky "obscured"; MET's fog = visibility under 1 km
                // (Locationforecast FAQ), so once the rule fires (fog >= 50 %) no line of sight reaches the horizon; the
                // fraction picked the rule, it is not an areal cover
                A = make((w.T - Td) < 2.f ? "1 Fog saturated" : "1 Fog", "Fog", 0.f, (w.T - Td) < 2.f ? 250.f : 120.f, 1.f, 'a'); r.notes.push_back("1/2 fog (St at the surface)");
                if (w.low > 0.1f) { R = make("2 St nebulosus", "St", 250.f, 500.f, w.low, 'r'); r.notes.push_back("2 St nebulosus lifting"); }
            }
        }
        else if (w.low > 0.f)
        {
            if (day && w.land && w.low < 0.6f)
            {
                genus = (w.hour < 13.f || w.low < 0.25f) ? "Cu_humilis" : "Cu_mediocris";
                const F32 top = cuBase + (genus == "Cu_humilis" ? 300.f : 1200.f);
                // the diurnal rule (section 9.4): none before late morning, peak mid-afternoon, gone after sunset
                const F32 diurnal = llclamp((w.hour - 9.f) / 4.f, 0.f, 1.f) * llclamp((19.5f - w.hour) / 2.f, 0.f, 1.f);
                // [WolfViewer 2026-10-14] but never below the EEP's own cloud share (Weather::eepLow). At dusk the fade took the
                // cumulus to 0.047 while the EEP still drew 44 % of the sky, so the thin Ac layer at 3 km became the largest and
                // was scaled up to the whole EEP cover: one height of flat, identical pancakes (Paul, 10-14 "flat and samey")
                R = make(genus == "Cu_humilis" ? "17 Cu_humilis" : "17 Cu_mediocris", genus, cuBase, top, std::max(w.low * diurnal, w.eepLow), 'r');
                r.notes.push_back("17 " + genus);
                if (w.wind >= 4.f && genus == "Cu_mediocris") r.notes.push_back("18 streets along the wind");
            }
            else if (day && w.land)
            {
                R = make("19 Sc", "Sc", 600.f, 1500.f, w.low, 'r'); r.notes.push_back("19 Sc cumulogenitus");
            }
            else
            {
                if (w.wind < 2.f && (w.T - Td) < 1.5f) { R = make("20 St", "St", 150.f, 500.f, w.low, 'r'); r.notes.push_back("20 St (calm, saturated)"); }
                else { R = make("21 Sc sheet", "Sc", 600.f, 1500.f, w.low, 'r'); r.notes.push_back(w.wind > 12.9f ? "22 Sc open cells" : "21 Sc closed-cell sheet"); }
            }
        }
        // ---- the middle band (g) ----
        if (G.height <= 0.f && w.medium > 0.f && !thunder)
        {
            if (w.medium >= 0.6f && w.high > 0.3f && w.precip == 0.f) { G = make("13 As", "As", 3000.f, 5500.f, w.medium, 'g'); r.notes.push_back("13 As translucidus -> opacus"); }
            else if (day && w.summer && w.hour < 12.f && w.thunderPct >= 20.f) { G = make("15 Ac castellanus", "Ac", 3000.f, 4200.f, w.medium, 'g'); r.notes.push_back("15 Ac castellanus (thunder later)"); }
            else { G = make("14 Ac", "Ac", 3000.f, 3800.f, w.medium, 'g'); r.notes.push_back("14 Ac"); }
        }
        // ---- the high band (b) ----
        if (B.height <= 0.f && w.high > 0.f)
        {
            if (w.high >= 0.86f) { B = make("11 Cs", "Cs", 7000.f, 9000.f, 1.f, 'b'); r.notes.push_back("11 Cs covering the sky"); }
            else if (w.highTrend > 0.05f) { B = make(w.high > 0.5f ? "9 Cs invading" : "9 Ci invading", w.high > 0.5f ? "Cs" : "Ci", 7500.f, 9500.f, w.high, 'b'); r.notes.push_back("9 Ci invading (warm front)"); }
            else if (w.medium < 0.1f && w.low < 0.1f && w.high <= 0.4f && w.highTrend == 0.f && w.wind < 4.f) { B = make("10 Cc", "Cc", 7500.f, 8200.f, w.high, 'b'); r.notes.push_back("10 Cc / Ci not invading"); }
            else { B = make("10 Ci", "Ci", 7500.f, 9500.f, w.high, 'b'); r.notes.push_back("10 Ci fibratus"); }
        }
        // the weather texture turned to the wind and stretched along it: cloud streets (rule 18, rolls "parallel to
        // the mean wind", section 9.3) and cirrus filaments (WMO Ci: "delicate filaments"; along the upper wind, taken
        // as the forecast's one wind); the stretch 4x is the rolls' elongation (one modelling number, genus.py)
        const bool streets = std::find(r.notes.begin(), r.notes.end(), std::string("18 streets along the wind")) != r.notes.end();
        // [2026-10-26] the streak lookup for the genera the table marks (GenusRow::streaks: Ci, Ci spissatus); Cs and Cc are veils / grains
        const auto bRow = tab.genera.find(B.genus);
        const bool cirrus = B.height > 0.f && bRow != tab.genera.end() && bRow->second.streaks;
        // [WolfViewer 2026-10-13] always along the wind, streets or not: switching between 0 and the wind's angle turned the
        // whole cloud field about the camera (Paul: "clouds are moving VERY VERY fast")
        r.rotationDeg = w.windFrom + 90.f;
        r.stretch = (streets || cirrus) ? tab.stretch : 1.f;   // [2026-10-26] wolf_clouds.xml constants.stretch
        // [WolfViewer 2026-10-14] the stretch belongs to the layers that have that shape: the cirrus layer (b), and the
        // low layer (r) when rule 18 makes streets. Every other layer is only turned (cloudsCommon.glsl sampleWeather).
        // Paul's log 10-14: L0 Cu_humilis / L1 Ac / L2 Ci at coverage 0.016, stretch 4 - the cumulus drawn 4x long.
        r.layerStretch[0] = streets ? 1.f : 0.f;
        // [WolfViewer 2026-10-26] the cirrus along the UPPER wind (its own lookup, kind 2), not the 10 m wind the streets follow.
        // Jimmy Olsen 10-26 (UiT webcam, Tromso 02:27): curved, feathery cirrus where we drew "two huge, smooth, straight, parallel
        // bands" - one global stretch along the surface wind. The cirrus is at 7.5-9.5 km, above the friction layer, whose wind
        // is veered from the 10 m wind in the north (backed in the south) by the cross-isobar angle and faster by the friction's
        // reduction (wolfcloudgenus.h UPPER_VEER_*, SURFACE_OVER_UPPER_*: Wikipedia Wind gradient, FAA AC 00-6).
        r.layerStretch[2] = cirrus ? 2.f : 0.f;
        {
            const F32 veer = w.land ? tab.upperVeerLandDeg : tab.upperVeerSeaDeg;   // [2026-10-26] wolf_clouds.xml
            const F32 sign = w.latitude >= 0.f ? 1.f : -1.f;   // veered (wind-from + angle) in the north, backed in the south
            r.upperWindFrom = fmodf(w.windFrom + sign * veer + 360.f, 360.f);
            r.upperWindSpeed = w.wind / (w.land ? tab.surfaceOverUpperLand : tab.surfaceOverUpperSea);
            r.upperRotationDeg = r.upperWindFrom + 90.f;   // as rotationDeg = windFrom + 90 above
        }
        return r;
    }

    // [WolfViewer 2026-10-14] how far a genus's column tops spread (cloudsCommon.glsl wolfLayerTower): 1 - the lowest top as a
    // share of the layer's depth. From the genera's own depth ranges (BEAUTY_RESEARCH.md 8.1 / 8.4, WMO International Cloud
    // Atlas): Cu mediocris "a few hundred metres to about 2 km" over its 1.2 km layer here, lowest top 300 m -> 1 - 0.25;
    // Cu congestus "+3-5 km" over its 5 km layer -> 1 - 0.6. Cu humilis keeps a level top ("tops nearly the same level", WMO
    // Cumulus humilis) and Cb its flat anvil ("top nearly always flattened", WMO Cumulonimbus): 0, takram's fixed band.
    F32 towerShareOf(const std::string& genus)
    {
        if (genus == "Cu_mediocris") return table().towerShareMediocris;   // [2026-10-26] wolf_clouds.xml constants
        if (genus == "Cu_congestus") return table().towerShareCongestus;
        return 0.f;
    }

    F32 towerShare(const Layer& l)
    {
        return l.height > 0.f ? l.tower : 0.f;   // eased with the layer's other numbers (approach)
    }

    void toCloudLayers(const Result& r, WolfTk::CloudLayers& out)
    {
        for (S32 i = 0; i < 4; ++i)
        {
            const Layer& l = r.layer[i];
            WolfTk::CloudLayer& c = out.layer[i];
            c.channel = l.channel;
            c.altitude = l.altitude; c.height = l.height;
            c.densityScale = l.densityScale;
            c.shapeAmount = l.shapeAmount; c.shapeDetailAmount = l.shapeDetailAmount; c.weatherExponent = l.weatherExponent;
            c.shapeAlteringBias = l.shapeAlteringBias; c.coverageFilterWidth = l.coverageFilterWidth;
            c.densityProfile = l.profile;
            c.coverage = l.height > 0.f ? l.coverage : 0.f;
            c.ice = l.ice; c.absorption = l.absorption;
            // [WolfViewer round 16c] the powder in takram's density units: its cumulus layers' densityScale 0.2 (CloudLayers.ts:36,
            // :48) over this layer's physical one (cloudsParameters.glsl wolfLayerPowder)
            c.powder = l.densityScale > 0.f ? 0.2f / l.densityScale : 1.f;
            c.shadow = l.shadow && l.height > 0.f;
            c.fibre = l.fibre;   // [2026-10-26] cloudsCommon.glsl wolfLayerFibre
            c.repeat = l.repeat; // [2026-10-26] cloudsCommon.glsl wolfLayerRepeat (held while the layer shows: approach)
            // [CLOUDS CLEAR OF BUILDS 2026-10-26] every genus fades round builds except ground fog (Paul: fog is weather you walk
            // through, not cloud cutting through roofs) - cloudsCommon.glsl wolfLayerBuildFade
            c.buildFade = l.genus == "Fog" ? 0.f : 1.f;
        }
    }

    namespace
    {
        void towards(F32& v, F32 target, F32 k) { v += (target - v) * k; }
    }

    void approach(Result& cur, const Result& target, F32 dt, F32 tau)
    {
        const F32 k = (dt > 0.f && tau > 0.f) ? 1.f - std::exp(-dt / tau) : 1.f;
        for (S32 i = 0; i < 4; ++i)
        {
            Layer& c = cur.layer[i]; const Layer& t = target.layer[i];
            // [WolfViewer 2026-10-14] which lookup a layer uses may change only while it shows nothing (audit B3: it was taken
            // once, at the first weather, so cirrus arriving later stayed unstretched)
            if (c.height <= 0.f) { cur.layerStretch[i] = target.layerStretch[i]; c.repeat = t.repeat; }   // [2026-10-26] the repeat too: it would re-form the clouds
            if (c.height <= 0.f && t.height > 0.f)
            {
                // a layer coming in: start it at the target's shape with no body, so it grows in place
                c = t; c.height = 0.f; c.coverage = 0.f;
            }
            if (t.height <= 0.f && c.height > 0.f)
            {
                // a layer going out: its numbers shrink to nothing where it stands
                towards(c.height, 0.f, k); towards(c.coverage, 0.f, k);
                if (c.height < 1.f) { c = t; c.channel = t.channel; cur.layerStretch[i] = target.layerStretch[i]; }
                continue;
            }
            c.genus = t.genus; c.channel = t.channel; c.shadow = t.shadow;
            towards(c.altitude, t.altitude, k); towards(c.height, t.height, k); towards(c.densityScale, t.densityScale, k);
            towards(c.shapeAmount, t.shapeAmount, k); towards(c.shapeDetailAmount, t.shapeDetailAmount, k); towards(c.weatherExponent, t.weatherExponent, k);
            towards(c.shapeAlteringBias, t.shapeAlteringBias, k); towards(c.coverageFilterWidth, t.coverageFilterWidth, k);
            towards(c.profile.expTerm, t.profile.expTerm, k); towards(c.profile.exponent, t.profile.exponent, k);
            towards(c.profile.linearTerm, t.profile.linearTerm, k); towards(c.profile.constantTerm, t.profile.constantTerm, k);
            towards(c.coverage, t.coverage, k); towards(c.ice, t.ice, k); towards(c.absorption, t.absorption, k);
            towards(c.tower, t.tower, k);   // [2026-10-14] a genus change grows or settles the tops, never jumps
            towards(c.fibre, t.fibre, k);   // [2026-10-26] and the fibres
        }
        // [WolfViewer 2026-10-13] the texture's turn and stretch are NOT animated (nor, 2026-10-14, which layers are stretched:
        // switching a layer between the two lookups would re-form its clouds in place).
        // Source: cloudsCommon.glsl:123 sampleWeather, wuv = wolfWeatherRotation * (uv - wolfWeatherPivot) + wolfWeatherPivot:
        // changing the turn or the stretch moves every cloud by (its distance from the pivot) x (the change), so a 90 deg turn
        // eased over GENUS_TAU (20 s) swept clouds 20 km away at ~1.5 km/s - the "clouds moving very very fast" Paul saw on
        // 10-13. A real cloud field does not rotate when the wind veers; new clouds form along the new wind. The turn and the
        // stretch are taken once, when the genus state starts (sGenusValid false: first frame or a sky reset,
        // wolfenvironmentrender.cpp:640/659/1061), and held; the only motion left is the drift at the wind's own speed
        // (localWeatherOffset, wolfenvironmentrender.cpp:737).
        // [WolfViewer 2026-10-14] audit B3: "taken once" froze them at the first weather of the session (sGenusValid is only
        // reset by a teleport). They now follow whenever nothing on screen would move: the stretch (it acts on the stretched
        // layers only) once no stretched layer has a body, the turn (it acts on every layer) once no layer has one.
        // [WolfViewer 2026-10-26] the upper turn (the cirrus lookup, kind 2) the same way: it moves only the layers read through it,
        // so it follows once none of them has a body. Kind 1 (the streets) is read through rotationDeg, which also turns the
        // unstretched layers (cloudsCommon.glsl wolfWeatherTurn), so rotationDeg still waits for every layer.
        bool stretchedShown = false, anyShown = false, upperShown = false;
        for (S32 i = 0; i < 4; ++i)
        {
            if (cur.layer[i].height > 0.f && cur.layer[i].coverage > 1e-3f)
            {
                anyShown = true;
                if (cur.layerStretch[i] > 0.5f) stretchedShown = true;
                if (cur.layerStretch[i] > 1.5f) upperShown = true;
            }
        }
        if (!stretchedShown) cur.stretch = target.stretch;
        if (!anyShown) cur.rotationDeg = target.rotationDeg;
        if (!upperShown) cur.upperRotationDeg = target.upperRotationDeg;
        // the drift follows the forecast always (a speed, not a turn: the clouds move on, nothing re-forms)
        cur.upperWindFrom = target.upperWindFrom; cur.upperWindSpeed = target.upperWindSpeed;
        cur.dewPoint = target.dewPoint; cur.lcl = target.lcl; cur.notes = target.notes;
    }

    // ---- [WolfViewer 2026-10-26] the cloud table (wolfcloudgenus.h Table) ----
    namespace
    {
        bool readNumber(const LLSD& m, const char* key, F32& out, bool required, const std::string& where, std::string& err)
        {
            if (!m.has(key)) { if (required) err = where + ": missing '" + key + "'"; return !required; }
            const LLSD& v = m[key];
            if (!(v.isReal() || v.isInteger())) { err = where + ": '" + key + "' is not a number"; return false; }
            const F32 f = (F32)v.asReal();
            if (!llfinite(f)) { err = where + ": '" + key + "' is not finite"; return false; }
            out = f;
            return true;
        }
        bool readBool(const LLSD& m, const char* key, bool& out, bool required, const std::string& where, std::string& err)
        {
            if (!m.has(key)) { if (required) err = where + ": missing '" + key + "'"; return !required; }
            const LLSD& v = m[key];
            if (!(v.isBoolean() || v.isInteger())) { err = where + ": '" + key + "' is not a boolean"; return false; }
            out = v.asBoolean();
            return true;
        }
    }

    const Table& table() { return mutableTable(); }
    U32 tableGeneration() { return mutableTable().generation; }

    bool loadTable()
    {
        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "wolf_clouds.xml");
        Table& cur = mutableTable();
        auto fail = [&](const std::string& why)
        {
            LL_WARNS("WolfEnv") << "wolf cloud table NOT loaded from " << path << ": " << why << " - keeping the "
                                << (cur.generation ? "previous table" : "compiled defaults") << LL_ENDL;
            return false;
        };
        llifstream file(path.c_str());
        if (!file.is_open()) return fail("cannot open the file");
        LLSD data;
        if (LLSDSerialize::fromXMLDocument(data, file) < 1 || !data.isMap()) return fail("not an LLSD XML map (a parse error above this line names the place)");
        Table t = defaultTable();   // the compiled defaults, overridden by what the file gives
        std::string err;
        if (data.has("constants"))
        {
            const LLSD& c = data["constants"];
            if (!c.isMap()) return fail("'constants' is not a map");
            struct { const char* key; F32* value; } keys[] = {
                { "low_mid_tile_m", &t.lowMidTileM }, { "cirrus_tile_m", &t.cirrusTileM }, { "stretch", &t.stretch },
                { "flow_bend_streets", &t.flowBendStreets }, { "flow_bend_upper", &t.flowBendUpper }, { "genus_tau_s", &t.genusTauS },
                { "build_clear_m0_m", &t.buildClearM0 }, { "build_clear_w_m", &t.buildClearW },
                { "fibre_period_across_m", &t.fibrePeriodAcrossM }, { "fibre_period_along_m", &t.fibrePeriodAlongM }, { "fibre_period_vertical_m", &t.fibrePeriodVerticalM },
                { "upper_veer_sea_deg", &t.upperVeerSeaDeg }, { "upper_veer_land_deg", &t.upperVeerLandDeg },
                { "surface_over_upper_sea", &t.surfaceOverUpperSea }, { "surface_over_upper_land", &t.surfaceOverUpperLand },
                { "tower_share_mediocris", &t.towerShareMediocris }, { "tower_share_congestus", &t.towerShareCongestus },
                { "cu_base_min_m", &t.cuBaseMinM }, { "cu_base_max_m", &t.cuBaseMaxM },
            };
            for (const auto& k : keys) if (!readNumber(c, k.key, *k.value, false, "constants", err)) return fail(err);
            if (!readBool(c, "coverage_per_layer", t.coveragePerLayer, false, "constants", err)) return fail(err);
            if (!(t.lowMidTileM >= 1000.f && t.cirrusTileM >= 1000.f)) return fail("constants: a tile under 1 km");
            if (!(t.stretch >= 1.f)) return fail("constants: stretch under 1");
            if (!(t.genusTauS > 0.f)) return fail("constants: genus_tau_s must be positive");
            if (!(t.buildClearM0 >= 0.f && t.buildClearW >= 1.f)) return fail("constants: build_clear_m0_m < 0 or build_clear_w_m < 1");
            if (!(t.fibrePeriodAcrossM > 0.f && t.fibrePeriodAlongM > 0.f && t.fibrePeriodVerticalM > 0.f)) return fail("constants: a fibre period is not positive");
            if (!(t.surfaceOverUpperSea > 0.f && t.surfaceOverUpperLand > 0.f)) return fail("constants: surface_over_upper must be positive");
            if (!(t.cuBaseMinM >= 0.f && t.cuBaseMaxM > t.cuBaseMinM)) return fail("constants: cu_base_min_m / cu_base_max_m out of order");
        }
        if (!data.has("genera") || !data["genera"].isMap()) return fail("no 'genera' map");
        {
            const LLSD& g = data["genera"];
            for (LLSD::map_const_iterator it = g.beginMap(); it != g.endMap(); ++it)
            {
                const std::string& name = it->first;
                const LLSD& m = it->second;
                const std::string where = "genera." + name;
                if (!m.isMap()) return fail(where + " is not a map");
                GenusRow row;
                if (!readNumber(m, "shape", row.shape, true, where, err)) return fail(err);
                if (!readNumber(m, "detail", row.detail, true, where, err)) return fail(err);
                if (!readNumber(m, "exponent", row.exponent, true, where, err)) return fail(err);
                if (!readNumber(m, "bias", row.bias, true, where, err)) return fail(err);
                if (!readNumber(m, "filter", row.filter, true, where, err)) return fail(err);
                if (!m.has("profile") || !m["profile"].isArray() || m["profile"].size() != 4) return fail(where + ": 'profile' must be an array of 4 numbers");
                {
                    F32 pv[4];
                    for (S32 k = 0; k < 4; ++k)
                    {
                        const LLSD& v = m["profile"][k];
                        if (!(v.isReal() || v.isInteger())) return fail(where + ": profile[" + std::to_string(k) + "] is not a number");
                        pv[k] = (F32)v.asReal();
                        if (!llfinite(pv[k])) return fail(where + ": profile[" + std::to_string(k) + "] is not finite");
                    }
                    row.profile.expTerm = pv[0]; row.profile.exponent = pv[1]; row.profile.linearTerm = pv[2]; row.profile.constantTerm = pv[3];
                }
                if (!readNumber(m, "lwc_g_m3", row.lwc, true, where, err)) return fail(err);
                if (!readNumber(m, "ice", row.ice, true, where, err)) return fail(err);
                if (!readNumber(m, "absorption", row.absorption, true, where, err)) return fail(err);
                if (!readBool(m, "shadow", row.shadow, true, where, err)) return fail(err);
                if (!readNumber(m, "fibre", row.fibre, true, where, err)) return fail(err);
                if (!readNumber(m, "repeat", row.repeat, false, where, err)) return fail(err);
                if (!readBool(m, "streaks", row.streaks, false, where, err)) return fail(err);
                if (!(row.lwc > 0.f)) return fail(where + ": lwc_g_m3 must be positive");
                if (!(row.filter > 0.f)) return fail(where + ": filter must be positive");
                if (row.ice < 0.f || row.ice > 1.f) return fail(where + ": ice must be 0..1");
                if (row.fibre < 0.f || row.fibre > 1.f) return fail(where + ": fibre must be 0..1");
                if (!(row.repeat >= 1.f) || fabsf(row.repeat - roundf(row.repeat)) > 1e-3f) return fail(where + ": repeat must be a whole number of 1 or more (the texture wraps whole)");
                row.repeat = roundf(row.repeat);
                t.genera[name] = row;
            }
        }
        // every genus the rules name must be there (map())
        static const char* REQUIRED[] = { "Cu_humilis", "Cu_mediocris", "Cu_congestus", "Cb", "Sc", "St", "Fog", "Ns", "Ac", "As", "Ci", "Ci_spissatus", "Cs", "Cc" };
        for (const char* name : REQUIRED) if (!t.genera.count(name)) return fail(std::string("genera: '") + name + "' is missing");
        if (data.has("heights"))
        {
            const LLSD& h = data["heights"];
            if (!h.isMap()) return fail("'heights' is not a map");
            for (LLSD::map_const_iterator it = h.beginMap(); it != h.endMap(); ++it)
            {
                const std::string where = "heights." + it->first;
                if (!it->second.isMap()) return fail(where + " is not a map");
                HeightRule rule;
                if (!readNumber(it->second, "base", rule.base, false, where, err)) return fail(err);
                if (!readNumber(it->second, "top", rule.top, false, where, err)) return fail(err);
                if (!readNumber(it->second, "depth", rule.depth, false, where, err)) return fail(err);
                if (rule.base < 0.f && rule.top < 0.f && rule.depth < 0.f) return fail(where + ": needs base, top or depth");
                if (rule.base >= 0.f && rule.top >= 0.f && rule.top <= rule.base) return fail(where + ": top is not above base");
                if (rule.depth >= 0.f && rule.depth < 1.f) return fail(where + ": depth under 1 m");
                t.heights[it->first] = rule;
            }
        }
        t.path = path;
        t.generation = cur.generation + 1;
        cur = t;
        LL_INFOS("WolfEnv") << "wolf cloud table loaded: " << path << " - " << t.genera.size() << " genera, " << t.heights.size()
                            << " height rules, generation " << t.generation << LL_ENDL;
        return true;
    }
}
