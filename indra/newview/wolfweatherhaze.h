/**
 * @file wolfweatherhaze.h
 * @brief [WolfViewer 2026-11-24 ticket 655] The world's haze extinction the real weather gives (rain, drizzle, snow, mist).
 *
 * Source: WolfSim WolfAutoEnvironmentModule.cs WeatherWorldSigma / Weather.IsWet (ticket 655, 24 Nov 2026) - the same
 * formula on both sides, keep them equal: the server bakes it into the region's EEP (distance_multiplier) for every viewer,
 * and this viewer holds the EEP's world haze to it (WolfEnvironmentRender::worldDistanceMultiplier).
 *
 * Jimmy Olsen, TROMSO, 8 Nov 2026 (ticket 655): real Tromsø in rain "has haze on horizon (caused by precipitation)", the
 * Wolf Environment's "too clean to look natural". Paul, 24 Nov: when there is rain, snow, mist or drizzle in the region's
 * real weather the horizon gets a paler, hazier look; when it is clear it stays as now.
 *
 * Header-only, no viewer types, so tests/wolfweatherhaze_test.cpp builds it alone.
 */
#ifndef WOLF_WEATHER_HAZE_H
#define WOLF_WEATHER_HAZE_H

#include <algorithm>
#include <cmath>
#include <string>

namespace WolfWeatherHaze
{
    /// UK CAA CAP 746 (Requirements for Meteorological Observations at Aerodromes) 4.67-4.68, 4.86: mist (BR) is reported
    /// for prevailing visibility 1000-5000 m due to water droplets, relative humidity at least 95 %; below 95 % it is haze
    /// (dry particles), which keeps today's look.
    constexpr float MIST_VISIBILITY_M = 5000.f;
    constexpr float MIST_HUMIDITY = 95.f;
    /// Under 1 km is fog (WMO), which the fog rules carry (fog % into the EEP and the viewer's own fog): the floor here.
    constexpr float FOG_VISIBILITY_M = 1000.f;
    /// -ln(0.05): the meteorological optical range is where a beam falls to 5 % (WMO-No. 8, Guide to Instruments and
    /// Methods of Observation, Vol. I ch. 9), extinction = 2.996 / range - Koschmieder with the 5 % threshold, as in
    /// Gultepe, Milbrandt and Binbin 2010, "Visibility parameterization for forecasting model applications", FOGDEW2010-112, Eq. 1.
    constexpr float MOR_LN = 2.9957323f;

    /// The weather as the web gives it (php/auto_env_lib.php observation: kind from MET's symbol, ae_symbol_weather; MET's
    /// precipitation_amount next 1 h, mm; Open-Meteo's hourly `visibility`, m, < 0 here when null).
    struct Weather
    {
        float fog = 0.f;              // 0..1
        std::string kind = "clear";   // clear | rain | snow (drizzle is MET's light rain)
        float precipitation = 0.f;    // mm/h
        float visibility = -1.f;      // m, < 0 unknown
        float humidity = 0.f;         // relative humidity % (the observation's)
    };

    /// Wet air: rain/drizzle/snow falling, or mist (visibility 5 km or less at 95 % RH or more). Source:
    /// WolfAutoEnvironmentModule.cs Weather.IsWet.
    inline bool isWet(const Weather& w)
    {
        if ((w.kind == "rain" || w.kind == "snow") && w.precipitation > 0.f) return true;
        return w.visibility > 0.f && w.visibility <= MIST_VISIBILITY_M && w.humidity >= MIST_HUMIDITY;
    }

    /// The world's haze extinction, 1/m, in the stock haze's terms ((blue_density.r + haze_density) x density_multiplier x
    /// distance_multiplier, [S] atmosphericsFuncs.glsl:79-87). Source: WolfAutoEnvironmentModule.cs WeatherWorldSigma.
    ///  Dry: EEP's default air (llsettingssky.cpp:1226-1231, ~52 km) under the server's fog rule (WolfEnvironment.cs
    ///       OverlayWeather: haze_density 70 % to 5, distance_multiplier x (1 + 4 fog)) and haze rule (ApplyDoc: 60 %, 1 + 3 haze).
    ///  Wet: the largest of that, the real visibility in mist air (RH >= 95 %; 2.996 / V, V >= 1 km) and the falling precipitation's own (Gultepe et
    ///       al. 2010 Table 2 rain/drizzle mean V_km = 9.01 - 4.12 R^0.176; Table 3 snow mean V_km = 1.10 R^-0.701; R mm/h;
    ///       held to 1..50 km). Extinction adds over species (their Eq. 6); the precipitation term is a floor beside the model
    ///       visibility, which may already hold it.
    inline float worldSigma(const Weather& w, float haze)
    {
        const float fog = std::clamp(w.fog, 0.f, 1.f);
        const float hz = std::clamp(haze, 0.f, 1.f);
        float hd = 0.7f + (5.f - 0.7f) * 0.7f * fog;   // WolfEnvironment.cs OverlayWeather
        float dist = 0.8f * (1.f + 4.f * fog);          // ditto
        hd += (5.f - hd) * 0.6f * hz;                   // WolfEnvironment.cs ApplyDoc
        dist *= 1.f + 3.f * hz;                         // ditto
        float sigma = (0.2447f + hd) * 0.0001f * dist;
        if (!isWet(w)) return sigma;
        // the visibility only in mist air (RH >= 95 %): lower, it is haze or a model artefact (24 Nov, Open-Meteo: Bergen 1.6 km
        // at 46 % RH, Narvik 240 m at 71 % under a clear sky); rain's and snow's own extinction is the term below
        if (w.visibility > 0.f && w.humidity >= MIST_HUMIDITY)
            sigma = std::max(sigma, MOR_LN / std::max(w.visibility, FOG_VISIBILITY_M));
        if ((w.kind == "rain" || w.kind == "snow") && w.precipitation > 0.f)
        {
            const float r = w.precipitation;
            float vKm = w.kind == "snow" ? 1.10f * std::pow(r, -0.701f)      // Gultepe et al. 2010 Table 3, mean
                                         : 9.01f - 4.12f * std::pow(r, 0.176f); // Table 2, mean
            vKm = std::clamp(vKm, FOG_VISIBILITY_M / 1000.f, 50.f);
            sigma = std::max(sigma, MOR_LN / (vKm * 1000.f));
        }
        return sigma;
    }

    /// The EEP's distance_multiplier the world uses: wet - set to the weather's haze (raised or lowered); dry - only held to
    /// it (an EEP already clearer keeps its own), exactly as the 2026-10-27 rule did. Wet is kept inside EEP's range
    /// 0.0001..1000 (llsettingssky.cpp validation). Source: WolfAutoEnvironmentModule.cs HoldWorldHazeToWeather.
    inline float distanceMultiplier(float eepSigma, float distanceMultiplier, float weatherSigma, bool wet)
    {
        if (weatherSigma < 0.f || eepSigma <= 0.f) return distanceMultiplier;
        if (!wet)
            return eepSigma <= weatherSigma ? distanceMultiplier : distanceMultiplier * (weatherSigma / eepSigma);
        return std::clamp(distanceMultiplier * (weatherSigma / eepSigma), 0.0001f, 1000.f);
    }
}

#endif // WOLF_WEATHER_HAZE_H
