/**
 * @file wolfcloudgenus.cpp
 * @brief The weather -> cloud genera mapping (see wolfcloudgenus.h for the sources). The twin of scratchpad
 *        phys2/genus.py: the rules and the per-genus rows are the same, line for line.
 */

#include "llviewerprecompiledheaders.h"
#include "wolfcloudgenus.h"

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

        /** One per-genus row (genus.py GENERA, section 8.4): style, shape amounts, exponent, bias, filter width,
         *  density profile (exp, exponent, linear, constant), LWC / IWC g/m3, ice, absorption, shadow. */
        struct Row
        {
            F32 sa, sda, we, sab, cfw;
            WolfTk::DensityProfile prof;
            F32 lwc, ice, absorption;
            bool shadow;
        };
        const std::map<std::string, Row>& rows()
        {
            static const std::map<std::string, Row> R = {
                // tops level: a flat profile, the semi-circle rounding only (WMO humilis)
                { "Cu_humilis",   { 1.0f, 1.0f, 1.0f, 0.25f, 0.6f, { 0.f, 0.f, 0.f, 1.f },      0.3f,   0.f,  0.f,   true } },
                // takram's default profile: domes
                { "Cu_mediocris", { 1.0f, 1.0f, 1.0f, 0.35f, 0.6f, { 0.f, 0.f, 0.75f, 0.25f },  0.5f,   0.f,  0.f,   true } },
                // towers: density growing to the top
                { "Cu_congestus", { 1.0f, 1.0f, 1.0f, 0.45f, 0.6f, { 0.f, 0.f, 1.f, 0.1f },     1.0f,   0.f,  0.f,   true } },
                // the anvil: an exponential flare towards the top; rain: absorption (Schneider 2015 p. 69)
                { "Cb",           { 1.0f, 1.0f, 1.0f, 0.6f,  0.7f, { 0.6f, 2.f, 0.f, 0.4f },    2.0f,   0.f,  0.02f, true } },
                // a slab of rolls / tessellations (takram's exponent 1 and filter width 0.6: a sharpened filter cut hard edges, round 8)
                { "Sc",           { 1.0f, 0.6f, 1.0f, 0.5f,  0.6f, { 0.f, 0.f, 0.f, 1.f },      0.45f,  0.f,  0.f,   true } },
                // a uniform thin sheet, no cauliflower
                { "St",           { 0.3f, 0.0f, 1.0f, 0.9f,  0.3f, { 0.f, 0.f, 0.f, 1.f },      0.25f,  0.f,  0.f,   true } },
                // the ground slab, in the shadow map (round 8): the march's optical depth to the light at a sample is two short
                // steps plus the shadow map's column (clouds.frag:528-545 marchOpticalDepth + sampleShadowOpticalDepth), so a
                // slab left out of the map was lit by the unattenuated beam throughout - the sun's warm colour, not the fog's grey
                { "Fog",          { 0.2f, 0.0f, 1.0f, 0.95f, 0.2f, { 0.f, 0.f, 0.f, 1.f },      0.25f,  0.f,  0.f,   true } },
                // a deep smooth slab, rain darkening; the sun blotted out by its depth
                { "Ns",           { 0.4f, 0.0f, 1.0f, 0.9f,  0.4f, { 0.f, 0.f, 0.f, 1.f },      0.3f,   0.f,  0.02f, true } },
                // thin elements 1-5 deg with shading (exponent 1.5: separate patches)
                { "Ac",           { 1.0f, 0.5f, 1.5f, 0.5f,  0.5f, { 0.f, 0.f, 0.f, 1.f },      0.3f,   0.f,  0.f,   true } },
                // a uniform sheet, ice at the top (section 8.1: three layers)
                { "As",           { 0.3f, 0.0f, 1.0f, 0.9f,  0.3f, { 0.f, 0.f, 0.f, 1.f },      0.3f,   0.4f, 0.f,   true } },
                // a thin veil eroded little; the filaments come from the texture stretched along the wind; ice; IWC 0.03
                { "Ci",           { 0.2f, 0.0f, 1.2f, 0.6f,  0.6f, { 0.f, 0.f, 0.f, 1.f },      0.03f,  1.f,  0.f,   false } },
                // a transparent veil: IWC 0.008 over 2 km -> tau 0.9 (section 8.4 tau <~ 1)
                { "Cs",           { 0.2f, 0.0f, 1.0f, 0.9f,  0.2f, { 0.f, 0.f, 0.f, 1.f },      0.008f, 1.f,  0.f,   false } },
                // grains < 1 deg (exponent 2: small separate elements)
                { "Cc",           { 0.8f, 0.0f, 2.0f, 0.5f,  0.4f, { 0.f, 0.f, 0.f, 1.f },      0.03f,  1.f,  0.f,   false } },
            };
            return R;
        }

        Layer make(const std::string& genus, F32 base, F32 top, F32 cover, char channel)
        {
            const Row& g = rows().at(genus);
            Layer l;
            l.genus = genus; l.channel = channel;
            l.altitude = base; l.height = std::max(top - base, 0.f);
            l.densityScale = g.ice >= 1.f ? sigmaIce(g.lwc) : sigmaWater(g.lwc);
            l.shapeAmount = g.sa; l.shapeDetailAmount = g.sda; l.weatherExponent = g.we; l.shapeAlteringBias = g.sab; l.coverageFilterWidth = g.cfw;
            l.profile = g.prof;
            l.coverage = llclamp(cover, 0.f, 1.f);
            l.ice = g.ice; l.absorption = g.absorption; l.shadow = g.shadow;
            l.tower = towerShareOf(genus);   // [2026-10-14] cloudsCommon.glsl wolfLayerTower
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
        F32 cuBase = llclamp(std::max(lcl, 200.f), 200.f, 2400.f);   // rule 4: max(125 (T - Td), 200 m); Cu up to 2400 m
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
            R = make("Cb", cuBase, 12000.f, std::max(w.low, 0.3f), 'r'); r.notes.push_back("4 Cb capillatus incus");
            B = make("Ci", 9000.f, 11000.f, std::max(w.high, 0.4f), 'b'); r.notes.push_back("4 anvil Ci spissatus");
        }
        else if (steady)
        {
            R = make("Ns", 500.f, 4000.f, 1.f, 'r'); r.notes.push_back("7 Ns + pannus");
            A = make("St", 150.f, 500.f, std::min(1.f, 0.5f + 0.5f * w.low), 'a'); r.notes.push_back("7 pannus (St fractus) below");
            G = make("As", 4000.f, 6000.f, std::max(w.medium, 0.8f), 'g'); r.notes.push_back("7 As above");
        }
        else if (w.precip > 0.f && showers(w.symbol))
        {
            if (day && w.summer)
            {
                const bool heavy = w.precip >= 0.95f;   // Factory.cpp precipitation limits {0.1, 0.25, 0.95} mm: heavy
                // [WolfViewer 2026-10-14] congestus "+3-5 km" deep (BEAUTY_RESEARCH.md 8.4; was +3 km): with its tower share
                // (towerShareOf, 0.6) the columns' tops spread over 3 to 5 km above the base instead of one flat 3 km slab
                R = make(heavy ? "Cb" : "Cu_congestus", cuBase, cuBase + (heavy ? 6000.f : 5000.f), std::max(w.low, 0.3f), 'r');
                r.notes.push_back("5 Cu congestus / Cb calvus showers");
            }
            else
            {
                R = make("Sc", 700.f, 1700.f, std::max(w.low, 0.5f), 'r');
                A = make("St", 300.f, 600.f, 0.3f * w.low, 'a');
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
            R = make("Cu_congestus", cuBase, cuBase + 5000.f, w.low, 'r');
            r.notes.push_back("4b Cu congestus (thunder forecast, no rain yet)");
        }
        else if (w.precip > 0.f && w.low >= 0.86f && w.medium < 0.3f)
        {
            const bool st = w.wind < 3.f;
            R = make(st ? "St" : "Sc", st ? 300.f : 500.f, st ? 700.f : 1500.f, w.low, 'r'); r.notes.push_back("8 St / Sc drizzle");
        }
        else if (foggy)
        {
            if (w.wind > 7.7f && !w.land)
            {
                R = make("Sc", 300.f, 1200.f, std::max(w.fog, w.low), 'r'); r.notes.push_back("3 advection fog lifted to Sc (wind > 7.7 m/s)");
            }
            else
            {
                // full cover (round 8b): BEAUTY_RESEARCH.md 9.4 rule 1 - the sky "obscured"; MET's fog = visibility under 1 km
                // (Locationforecast FAQ), so once the rule fires (fog >= 50 %) no line of sight reaches the horizon; the
                // fraction picked the rule, it is not an areal cover
                A = make("Fog", 0.f, (w.T - Td) < 2.f ? 250.f : 120.f, 1.f, 'a'); r.notes.push_back("1/2 fog (St at the surface)");
                if (w.low > 0.1f) { R = make("St", 250.f, 500.f, w.low, 'r'); r.notes.push_back("2 St nebulosus lifting"); }
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
                R = make(genus, cuBase, top, std::max(w.low * diurnal, w.eepLow), 'r');
                r.notes.push_back("17 " + genus);
                if (w.wind >= 4.f && genus == "Cu_mediocris") r.notes.push_back("18 streets along the wind");
            }
            else if (day && w.land)
            {
                R = make("Sc", 600.f, 1500.f, w.low, 'r'); r.notes.push_back("19 Sc cumulogenitus");
            }
            else
            {
                if (w.wind < 2.f && (w.T - Td) < 1.5f) { R = make("St", 150.f, 500.f, w.low, 'r'); r.notes.push_back("20 St (calm, saturated)"); }
                else { R = make("Sc", 600.f, 1500.f, w.low, 'r'); r.notes.push_back(w.wind > 12.9f ? "22 Sc open cells" : "21 Sc closed-cell sheet"); }
            }
        }
        // ---- the middle band (g) ----
        if (G.height <= 0.f && w.medium > 0.f && !thunder)
        {
            if (w.medium >= 0.6f && w.high > 0.3f && w.precip == 0.f) { G = make("As", 3000.f, 5500.f, w.medium, 'g'); r.notes.push_back("13 As translucidus -> opacus"); }
            else if (day && w.summer && w.hour < 12.f && w.thunderPct >= 20.f) { G = make("Ac", 3000.f, 4200.f, w.medium, 'g'); r.notes.push_back("15 Ac castellanus (thunder later)"); }
            else { G = make("Ac", 3000.f, 3800.f, w.medium, 'g'); r.notes.push_back("14 Ac"); }
        }
        // ---- the high band (b) ----
        if (B.height <= 0.f && w.high > 0.f)
        {
            if (w.high >= 0.86f) { B = make("Cs", 7000.f, 9000.f, 1.f, 'b'); r.notes.push_back("11 Cs covering the sky"); }
            else if (w.highTrend > 0.05f) { B = make(w.high > 0.5f ? "Cs" : "Ci", 7500.f, 9500.f, w.high, 'b'); r.notes.push_back("9 Ci invading (warm front)"); }
            else if (w.medium < 0.1f && w.low < 0.1f && w.high <= 0.4f && w.highTrend == 0.f && w.wind < 4.f) { B = make("Cc", 7500.f, 8200.f, w.high, 'b'); r.notes.push_back("10 Cc / Ci not invading"); }
            else { B = make("Ci", 7500.f, 9500.f, w.high, 'b'); r.notes.push_back("10 Ci fibratus"); }
        }
        // the weather texture turned to the wind and stretched along it: cloud streets (rule 18, rolls "parallel to
        // the mean wind", section 9.3) and cirrus filaments (WMO Ci: "delicate filaments"; along the upper wind, taken
        // as the forecast's one wind); the stretch 4x is the rolls' elongation (one modelling number, genus.py)
        const bool streets = std::find(r.notes.begin(), r.notes.end(), std::string("18 streets along the wind")) != r.notes.end();
        const bool cirrus = B.genus == "Ci" || B.genus == "Cs";
        // [WolfViewer 2026-10-13] always along the wind, streets or not: switching between 0 and the wind's angle turned the
        // whole cloud field about the camera (Paul: "clouds are moving VERY VERY fast")
        r.rotationDeg = w.windFrom + 90.f;
        r.stretch = (streets || cirrus) ? 4.f : 1.f;
        // [WolfViewer 2026-10-14] the stretch belongs to the layers that have that shape: the cirrus layer (b), and the
        // low layer (r) when rule 18 makes streets. Every other layer is only turned (cloudsCommon.glsl sampleWeather).
        // Paul's log 10-14: L0 Cu_humilis / L1 Ac / L2 Ci at coverage 0.016, stretch 4 - the cumulus drawn 4x long.
        r.layerStretch[0] = streets ? 1.f : 0.f;
        r.layerStretch[2] = cirrus ? 1.f : 0.f;
        return r;
    }

    // [WolfViewer 2026-10-14] how far a genus's column tops spread (cloudsCommon.glsl wolfLayerTower): 1 - the lowest top as a
    // share of the layer's depth. From the genera's own depth ranges (BEAUTY_RESEARCH.md 8.1 / 8.4, WMO International Cloud
    // Atlas): Cu mediocris "a few hundred metres to about 2 km" over its 1.2 km layer here, lowest top 300 m -> 1 - 0.25;
    // Cu congestus "+3-5 km" over its 5 km layer -> 1 - 0.6. Cu humilis keeps a level top ("tops nearly the same level", WMO
    // Cumulus humilis) and Cb its flat anvil ("top nearly always flattened", WMO Cumulonimbus): 0, takram's fixed band.
    F32 towerShareOf(const std::string& genus)
    {
        if (genus == "Cu_mediocris") return 1.f - 0.25f;
        if (genus == "Cu_congestus") return 1.f - 0.6f;
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
            if (c.height <= 0.f) cur.layerStretch[i] = target.layerStretch[i];
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
        bool stretchedShown = false, anyShown = false;
        for (S32 i = 0; i < 4; ++i)
        {
            if (cur.layer[i].height > 0.f && cur.layer[i].coverage > 1e-3f)
            {
                anyShown = true;
                if (cur.layerStretch[i] > 0.5f) stretchedShown = true;
            }
        }
        if (!stretchedShown) cur.stretch = target.stretch;
        if (!anyShown) cur.rotationDeg = target.rotationDeg;
        cur.dewPoint = target.dewPoint; cur.lcl = target.lcl; cur.notes = target.notes;
    }
}
