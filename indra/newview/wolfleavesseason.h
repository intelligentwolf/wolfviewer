/**
 * @file wolfleavesseason.h
 * @brief WolfViewer: the season of the falling leaves (ticket 718) - plain C++ with no viewer types, so the same rules can be
 *        checked outside the viewer (region-terrain-work/falling-leaves-2026-11-10/test/) and kept in step with WolfStorm's
 *        js/world/wolf_leaves.js by hand.
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

#ifndef WOLF_LEAVES_SEASON_H
#define WOLF_LEAVES_SEASON_H

#include <cctype>
#include <cmath>
#include <string>

/**
 * THE PLACE AND THE MOMENT come from what the grid already sends every viewer each minute, the `autoEnv` block of weather.php
 * (wolfregionweather.cpp readAutoEnv): the place's latitude, and skyShift, the seconds from now to the moment the region's sky
 * shows. They are read exactly as the grid's own seasonal features read them:
 *   - the latitude: wolfstorm/php/wolf_ambient_lib.php wa_place_lat - the automatic environment always has a place (London
 *     unless the owner chose another); a Wolf Environment has one unless it is "No place" at 0, 0. No place: null.
 *   - the day: wa_sounds_payload's `$at = time() + skyShift` and `gmdate('z', $at) + 1` - the UTC day of the year at the moment
 *     the sky shows (WolfRegionWeather::autoEnvSkyTime).
 *   - the hemisphere: south of the equator the year runs half a year on (wa_insect_season's peak - 182.5 days; wolfbirds.cpp
 *     murmuration_season / tern_season flip the months the same way).
 *   - the climate zone: wa_birds_zone_auto's WA_TROPIC_LAT and WA_POLAR_LAT (t715).
 * No place (an EEP day cycle has no seasons and no latitude): the northern calendar on today's date, as wolfbirds.cpp does
 * (southern() is false without a place).
 *
 * THE SEASONS (every figure CHOSEN for the look; Jimmy's ticket asks for "subtle", colours "accordingly to season" and "no
 * particles at all at winter"):
 *   - temperate (|lat| < WA_POLAR_LAT): the meteorological seasons on the northern-equivalent day - spring 1 Mar .. 31 May
 *     (a few fresh green leaves), summer 1 Jun .. 31 Aug (a few green ones), autumn 1 Sep .. 30 Nov (the most, turning from
 *     green through yellow, orange and red to brown as it goes on), winter 1 Dec .. end Feb (none).
 *   - polar (|lat| >= WA_POLAR_LAT): a short year - summer 15 May .. 31 Jul (green), autumn 1 Aug .. 30 Sep (the birch
 *     yellows and browns), winter the rest (none).
 *   - tropical (|lat| < WA_TROPIC_LAT): no winter - evergreen trees drop old leaves all the year, a few at a time, mostly
 *     green with some yellow and brown.
 */
namespace WolfLeavesSeason
{
    enum Kind { WINTER = 0, SPRING = 1, SUMMER = 2, AUTUMN = 3, TROPICAL = 4 };
    enum Colour { FRESH_GREEN = 0, GREEN, YELLOW, ORANGE, RED, BROWN, COLOURS };

    /** Leaf colours (linear 0..1 vertex colours, multiplied by the scene's light; chosen). */
    constexpr float PALETTE[COLOURS][3] = {
        { 0.45f, 0.62f, 0.20f },   // FRESH_GREEN - a new spring leaf, yellow-green
        { 0.24f, 0.40f, 0.12f },   // GREEN       - a summer leaf
        { 0.82f, 0.62f, 0.14f },   // YELLOW      - birch and beech gold
        { 0.78f, 0.38f, 0.10f },   // ORANGE      - beech and oak copper
        { 0.60f, 0.15f, 0.08f },   // RED         - maple and cherry
        { 0.45f, 0.30f, 0.14f },   // BROWN       - a dry oak leaf
    };

    // Source: wolfstorm/php/wolf_ambient_lib.php WA_TROPIC_LAT, WA_POLAR_LAT (t715, 2026-11-10).
    constexpr double TROPIC_LAT = 23.4358;
    constexpr double POLAR_LAT = 66.5642;
    // Source: wolf_ambient_lib.php wa_insect_season - the southern year runs half a year on (182.5 days there; 183 here, so the
    // southern 1 June lands on the northern 1 December and the southern winter starts with June, as it does).
    constexpr int SOUTH_SHIFT_DAYS = 183;

    // Day windows on the northern-equivalent day of a 365-day year (1 = 1 January; 60 = 1 March, 152 = 1 June,
    // 244 = 1 September, 335 = 1 December; 135 = 15 May, 213 = 1 August, 274 = 1 October).
    constexpr int SPRING_FIRST = 60, SUMMER_FIRST = 152, AUTUMN_FIRST = 244, WINTER_FIRST = 335;
    constexpr int POLAR_SUMMER_FIRST = 135, POLAR_AUTUMN_FIRST = 213, POLAR_WINTER_FIRST = 274;

    // How many leaves fall, as a share of an autumn tree's most (chosen: "subtle" - a few in spring and summer).
    constexpr float RATE_SPRING = 0.15f, RATE_SUMMER = 0.2f, RATE_TROPICAL = 0.3f;
    constexpr float RATE_AUTUMN_EDGE = 0.35f;   // at the start and end of autumn; 1 in the middle

    struct Season
    {
        Kind kind = WINTER;
        float rate = 0.f;                  // 0 .. 1: share of the most leaves a tree drops (0 in winter)
        float progress = 0.f;              // 0 .. 1 through the season (autumn's colours follow it)
        float weights[COLOURS] = { 0.f };  // how likely each colour is (sum 1, or all 0 in winter)
    };

    /** Days since 1970-01-01 to a civil date (Howard Hinnant's days_from_civil, public domain). */
    inline long days_from_civil(long y, unsigned m, unsigned d)
    {
        y -= m <= 2;
        const long era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = (unsigned)(y - era * 400);
        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + (long)doe - 719468;
    }

    /** The UTC day of the year (1 .. 366) of a moment in seconds since 1970 (PHP gmdate('z') + 1). */
    inline int dayOfYearUTC(double seconds)
    {
        const long days = (long)std::floor(seconds / 86400.0);
        // civil_from_days (Hinnant): the year of `days`
        const long z = days + 719468;
        const long era = (z >= 0 ? z : z - 146096) / 146097;
        const unsigned doe = (unsigned)(z - era * 146097);
        const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        long y = (long)yoe + era * 400;
        const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const unsigned mp = (5 * doy + 2) / 153;
        const unsigned m = mp < 10 ? mp + 3 : mp - 9;
        if (m <= 2) ++y;
        return (int)(days - days_from_civil(y, 1, 1)) + 1;
    }

    /** The day of the year a northern place would be at: the same day north of the equator (and with no place), half a
     *  year on south of it. Day 366 of a leap year counts as 365. */
    inline int northernDay(bool place_known, double lat, int doy)
    {
        int d = doy < 1 ? 1 : (doy > 365 ? 365 : doy);
        if (place_known && lat < 0.0) d = (d - 1 + SOUTH_SHIFT_DAYS) % 365 + 1;
        return d;
    }

    inline void setWeights(Season& s, float fresh, float green, float yellow, float orange, float red, float brown)
    {
        const float sum = fresh + green + yellow + orange + red + brown;
        const float k = sum > 0.f ? 1.f / sum : 0.f;
        s.weights[FRESH_GREEN] = fresh * k;
        s.weights[GREEN] = green * k;
        s.weights[YELLOW] = yellow * k;
        s.weights[ORANGE] = orange * k;
        s.weights[RED] = red * k;
        s.weights[BROWN] = brown * k;
    }

    /** Autumn at `f` (0 .. 1 through it): green early, yellow and orange, red in the middle, brown late. */
    inline void autumn(Season& s, float f)
    {
        const float pi = 3.14159265f;
        s.kind = AUTUMN;
        s.progress = f;
        s.rate = RATE_AUTUMN_EDGE + (1.f - RATE_AUTUMN_EDGE) * std::sin(pi * f);
        setWeights(s, 0.f, 0.5f * (1.f - f) * (1.f - f), 0.35f * (1.f - 0.5f * f), 0.25f, 0.05f + 0.2f * std::sin(pi * f),
                   0.05f + 0.55f * f * f);
    }

    /** The season of the leaves at a place on a day: `place_known` false = no place (northern calendar); `doy` the UTC day of
     *  the year (1 .. 366) at the moment the sky shows. */
    inline Season at(bool place_known, double lat, int doy)
    {
        Season s;
        const double a = place_known ? std::fabs(lat) : 51.5;   // no place: a temperate year (the default place is London)
        if (a < TROPIC_LAT)
        {
            s.kind = TROPICAL;
            s.rate = RATE_TROPICAL;
            s.progress = 0.f;
            setWeights(s, 0.f, 0.6f, 0.25f, 0.f, 0.f, 0.15f);
            return s;
        }
        const int d = northernDay(place_known, lat, doy);
        if (a >= POLAR_LAT)
        {
            if (d >= POLAR_SUMMER_FIRST && d < POLAR_AUTUMN_FIRST)
            {
                s.kind = SUMMER;
                s.rate = RATE_SUMMER;
                s.progress = (float)(d - POLAR_SUMMER_FIRST) / (float)(POLAR_AUTUMN_FIRST - POLAR_SUMMER_FIRST);
                setWeights(s, 0.1f, 0.8f, 0.1f, 0.f, 0.f, 0.f);
                return s;
            }
            if (d >= POLAR_AUTUMN_FIRST && d < POLAR_WINTER_FIRST)
            {
                autumn(s, (float)(d - POLAR_AUTUMN_FIRST) / (float)(POLAR_WINTER_FIRST - POLAR_AUTUMN_FIRST));
                return s;
            }
            return s;   // winter
        }
        if (d >= SPRING_FIRST && d < SUMMER_FIRST)
        {
            s.kind = SPRING;
            s.rate = RATE_SPRING;
            s.progress = (float)(d - SPRING_FIRST) / (float)(SUMMER_FIRST - SPRING_FIRST);
            setWeights(s, 0.85f, 0.15f, 0.f, 0.f, 0.f, 0.f);
            return s;
        }
        if (d >= SUMMER_FIRST && d < AUTUMN_FIRST)
        {
            s.kind = SUMMER;
            s.rate = RATE_SUMMER;
            s.progress = (float)(d - SUMMER_FIRST) / (float)(AUTUMN_FIRST - SUMMER_FIRST);
            setWeights(s, 0.1f, 0.8f, 0.1f, 0.f, 0.f, 0.f);
            return s;
        }
        if (d >= AUTUMN_FIRST && d < WINTER_FIRST)
        {
            autumn(s, (float)(d - AUTUMN_FIRST) / (float)(WINTER_FIRST - AUTUMN_FIRST));
            return s;
        }
        return s;   // winter
    }

    // ── THE KIND OF TREE ──────────────────────────────────────────────────────────────────────────────────────────────
    // Paul 2026-11-10 on the first test (a larch): "you have got a pine tree and are using the wrong type of leaves". A
    // conifer or other evergreen does not shed coloured autumn leaves: at most the odd brown needle.
    //   - the DESCRIPTION says it, after "wolftree": any of CONIFER_WORDS ("wolftree pine", "wolftree conifer", "wolftree
    //     spruce" ...) = an evergreen; any of BROADLEAF_WORDS ("wolftree oak", "wolftree broadleaf" ...) = a broadleaf;
    //   - otherwise the object's own NAME decides the same way (Jimmy's "Scots Pine Tree V2", "European Oak tree");
    //   - otherwise a broadleaf (plain "wolftree").
    // Whole words only (a "fire pit" is not a fir, a "pineapple" not a pine), with a plural s/es. Texture names are not used:
    // a viewer knows a face's texture by its UUID only.
    enum TreeKind { BROADLEAF = 0, CONIFER = 1 };
    inline const char* const* coniferWords()
    {
        static const char* const w[] = { "pine", "conifer", "fir", "spruce", "cedar", "evergreen", "larch", "hemlock", "cypress",
                                         "yew", "juniper", "sequoia", "redwood", "needle", "needles", nullptr };
        return w;
    }
    inline const char* const* broadleafWords()
    {
        static const char* const w[] = { "broadleaf", "deciduous", "oak", "maple", "beech", "birch", "elm", "willow", "chestnut",
                                         "sycamore", "poplar", "alder", "aspen", "hornbeam", "rowan", "cherry", "apple", "walnut",
                                         "linden", nullptr };
        return w;
    }
    /** Does the text (already lower case) hold one of `words` as a whole word, or with a plural s / es? */
    inline bool hasWord(const std::string& text, const char* const* words)
    {
        size_t i = 0;
        while (i < text.size())
        {
            while (i < text.size() && !std::isalpha((unsigned char)text[i])) ++i;
            size_t j = i;
            while (j < text.size() && std::isalpha((unsigned char)text[j])) ++j;
            if (j > i)
            {
                const std::string tok = text.substr(i, j - i);
                for (const char* const* w = words; *w; ++w)
                {
                    const std::string ww(*w);
                    if (tok == ww || tok == ww + "s" || tok == ww + "es") return true;
                }
            }
            i = j;
        }
        return false;
    }
    /** The kind of tree from its description and its name (both lower case). */
    inline TreeKind treeKind(const std::string& description_lower, const std::string& name_lower)
    {
        if (hasWord(description_lower, coniferWords())) return CONIFER;
        if (hasWord(description_lower, broadleafWords())) return BROADLEAF;
        if (hasWord(name_lower, coniferWords())) return CONIFER;
        return BROADLEAF;
    }

    // A conifer's needles: the odd brown one, a share of a broadleaf autumn's most (chosen: "a very occasional brown needle");
    // most in the autumn, none in the winter (as every tree here), the same all the year in the tropics.
    constexpr float NEEDLE_RATE_AUTUMN = 0.06f, NEEDLE_RATE = 0.03f;
    inline float needleRate(Kind k) { return k == WINTER ? 0.f : (k == AUTUMN ? NEEDLE_RATE_AUTUMN : NEEDLE_RATE); }
    constexpr int NEEDLE_COLOURS = 3;
    constexpr float NEEDLE_PALETTE[NEEDLE_COLOURS][3] = {
        { 0.50f, 0.33f, 0.15f },   // rust brown, a needle just fallen
        { 0.40f, 0.27f, 0.13f },   // brown
        { 0.58f, 0.45f, 0.26f },   // faded tan
    };

    // ── THE SHAPES (shared outlines; chosen, after real leaves) ───────────────────────────────────────────────────────
    // Counter-clockwise round the leaf's middle seen from its face; x along its length (tip +0.5, stalk -0.5) in lengths, y
    // across in half-widths. Star-shaped about (0, 0), so a fan from the middle fills each.
    //   OAK    Quercus robur: four rounded lobes a side with deep notches, the stalk short.
    //   BEECH  Fagus / birch / lime: an oval with a drawn-out tip and slightly toothed edges.
    //   NEEDLE a conifer needle: the BEECH outline drawn very thin (NEEDLE_WIDTH of its length).
    enum Shape { OAK = 0, BEECH = 1, NEEDLE = 2 };
    constexpr int OUTLINE_POINTS = 14;
    constexpr float OUTLINES[2][OUTLINE_POINTS][2] = {
        { { 0.50f, 0.f }, { 0.36f, 0.55f }, { 0.24f, 0.30f }, { 0.10f, 0.90f }, { -0.04f, 0.50f }, { -0.20f, 0.80f }, { -0.38f, 0.38f },
          { -0.50f, 0.f }, { -0.38f, -0.38f }, { -0.20f, -0.80f }, { -0.04f, -0.50f }, { 0.10f, -0.90f }, { 0.24f, -0.30f }, { 0.36f, -0.55f } },
        { { 0.50f, 0.f }, { 0.36f, 0.40f }, { 0.22f, 0.78f }, { 0.05f, 1.00f }, { -0.14f, 0.96f }, { -0.30f, 0.76f }, { -0.43f, 0.40f },
          { -0.50f, 0.f }, { -0.43f, -0.40f }, { -0.30f, -0.76f }, { -0.14f, -0.96f }, { 0.05f, -1.00f }, { 0.22f, -0.78f }, { 0.36f, -0.40f } },
    };
    constexpr float NEEDLE_WIDTH = 0.1f;

    inline const char* name(Kind k)
    {
        switch (k)
        {
            case SPRING: return "spring";
            case SUMMER: return "summer";
            case AUTUMN: return "autumn";
            case TROPICAL: return "tropical (all the year)";
            default: return "winter";
        }
    }
}

#endif // WOLF_LEAVES_SEASON_H
