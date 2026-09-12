/**
 * @file wolfweatherprofile.cpp
 * @brief WolfViewer: the weather profile — mirror of wolfstorm/js/world/weather_profile.js.
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

#include "wolfweatherprofile.h"

#include "llstring.h"

namespace
{
    // Source: weather_profile.js LEVELS — four stops, not the design's three, because the
    // engine and the "wolfrain<N>" script keyword have always had four and showing three would
    // make level 4 unreachable from the tab.
    struct LevelRow { const char* mRain; const char* mSnow; const char* mDensity; };
    const LevelRow LEVELS[5] = {
        { "",              "",            ""                },
        { "Light Drizzle", "Light Snow",  "Low density"     },
        { "Medium Rain",   "Snow",        "Medium density"  },
        { "Heavy Rain",    "Heavy Snow",  "High density"    },
        { "Torrential",    "Blizzard",    "Maximum density" },
    };

    S32 clamp_int(S32 v, S32 lo, S32 hi) { return v < lo ? lo : (v > hi ? hi : v); }

    /// An LLSD field that may be absent: absent keeps `dflt`, never becomes 0.
    S32 sd_int(const LLSD& sd, const char* key, S32 dflt)
    {
        return (sd.has(key) && sd[key].isDefined()) ? sd[key].asInteger() : dflt;
    }
    bool sd_bool(const LLSD& sd, const char* key, bool dflt)
    {
        return (sd.has(key) && sd[key].isDefined()) ? sd[key].asBoolean() : dflt;
    }
    std::string sd_str(const LLSD& sd, const char* key, const std::string& dflt)
    {
        if (!sd.has(key) || !sd[key].isDefined()) return dflt;
        const std::string s = sd[key].asString();
        return s.empty() ? dflt : s;
    }

    bool in_list(const std::vector<WolfWeatherProfile::Preset>& list, const std::string& id)
    {
        for (const auto& p : list) { if (id == p.mId) return true; }
        return false;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// colours
// ═══════════════════════════════════════════════════════════════════════════════════════════

// static
LLColor4 WolfWeatherProfile::neutralColor(Kind k)
{
    // rain #aaccff, snow #ffffff — the same two constants as weather_profile.js NEUTRAL.
    return (k == SNOW) ? LLColor4(1.f, 1.f, 1.f, 1.f)
                       : LLColor4(0xaa / 255.f, 0xcc / 255.f, 1.f, 1.f);
}

// static
std::string WolfWeatherProfile::colorToHex(const LLColor4& c)
{
    auto byte = [](F32 v) -> S32 { return clamp_int((S32)llround(v * 255.f), 0, 255); };
    return llformat("#%02x%02x%02x", byte(c.mV[VRED]), byte(c.mV[VGREEN]), byte(c.mV[VBLUE]));
}

// static
LLColor4 WolfWeatherProfile::colorFromHex(const std::string& hex, const LLColor4& dflt)
{
    if (hex.size() != 7 || hex[0] != '#') return dflt;
    for (size_t i = 1; i < 7; ++i)
    {
        const char c = (char)tolower(hex[i]);
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return dflt;
    }
    const S32 v = (S32)strtol(hex.c_str() + 1, nullptr, 16);
    return LLColor4(((v >> 16) & 0xff) / 255.f, ((v >> 8) & 0xff) / 255.f, (v & 0xff) / 255.f, 1.f);
}

LLColor4 WolfWeatherProfile::drawColor() const
{
    const LLColor4 base = neutralColor(mKind);
    const F32 t = (F32)clamp_int(mTintAmount, TINT_MIN, TINT_MAX) / 100.f;
    const F32 k = (F32)clamp_int(mBrightness, BRIGHTNESS_MIN, BRIGHTNESS_MAX) / 100.f;
    return LLColor4((base.mV[VRED]   + (mTint.mV[VRED]   - base.mV[VRED])   * t) * k,
                    (base.mV[VGREEN] + (mTint.mV[VGREEN] - base.mV[VGREEN]) * t) * k,
                    (base.mV[VBLUE]  + (mTint.mV[VBLUE]  - base.mV[VBLUE])  * t) * k,
                    1.f);
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// names
// ═══════════════════════════════════════════════════════════════════════════════════════════

// static
const char* WolfWeatherProfile::kindName(Kind k)
{
    switch (k) { case RAIN: return "rain"; case SNOW: return "snow"; default: return "clear"; }
}

// static
WolfWeatherProfile::Kind WolfWeatherProfile::kindFromName(const std::string& s)
{
    std::string l(s);
    LLStringUtil::toLower(l);
    if (l == "rain") return RAIN;
    if (l == "snow") return SNOW;
    return CLEAR;
}

// static
std::string WolfWeatherProfile::levelLabel(Kind k, S32 level)
{
    const LevelRow& r = LEVELS[clamp_int(level, 1, 4)];
    return (k == SNOW) ? r.mSnow : r.mRain;
}

// static
std::string WolfWeatherProfile::levelDensity(S32 level)
{
    return LEVELS[clamp_int(level, 1, 4)].mDensity;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// validation and transport
// ═══════════════════════════════════════════════════════════════════════════════════════════

void WolfWeatherProfile::clampAll()
{
    mLevel      = clamp_int(mLevel, 1, 4);
    mBrightness = clamp_int(mBrightness, BRIGHTNESS_MIN, BRIGHTNESS_MAX);
    mTintAmount = clamp_int(mTintAmount, TINT_MIN, TINT_MAX);
    mMoveSpeed  = clamp_int(mMoveSpeed, MOVE_MIN, MOVE_MAX);
    mDensity    = clamp_int(mDensity, DENSITY_MIN, DENSITY_MAX);
    mVelocity   = clamp_int(mVelocity, VELOCITY_MIN, VELOCITY_MAX);
    mSize       = clamp_int(mSize, PSIZE_MIN, PSIZE_MAX);
    mVolume     = clamp_int(mVolume, VOLUME_MIN, VOLUME_MAX);
    if (!in_list(soundPresets(), mSoundPreset)) mSoundPreset = "thunder";
    if (!in_list(soundLayers(), mSoundLayer))   mSoundLayer  = "soft";
}

void WolfWeatherProfile::fromLLSD(const LLSD& sd)
{
    if (!sd.isMap()) return;
    // A null / absent kind is "the region has no opinion" and is NOT turned into clear here;
    // the caller decides what an opinionless region means. An explicit "clear" is a choice.
    if (sd.has("kind") && sd["kind"].isDefined() && !sd["kind"].asString().empty())
    {
        mKind = kindFromName(sd["kind"].asString());
    }
    mLevel       = sd_int(sd, "level", mLevel);
    mEnabled     = sd_bool(sd, "enabled", mEnabled);
    mBrightness  = sd_int(sd, "brightness", mBrightness);
    mTint        = colorFromHex(sd_str(sd, "tintColor", colorToHex(mTint)), mTint);
    mTintAmount  = sd_int(sd, "tintAmount", mTintAmount);
    mMoveSpeed   = sd_int(sd, "moveSpeed", mMoveSpeed);
    mDensity     = sd_int(sd, "density", mDensity);
    mVelocity    = sd_int(sd, "velocity", mVelocity);
    mSize        = sd_int(sd, "size", mSize);
    mSound       = sd_bool(sd, "sound", mSound);
    mVolume      = sd_int(sd, "volume", mVolume);
    mSoundPreset = sd_str(sd, "soundPreset", mSoundPreset);
    mSoundLayer  = sd_str(sd, "soundLayer", mSoundLayer);
    clampAll();
}

LLSD WolfWeatherProfile::toLLSD() const
{
    LLSD sd;
    sd["kind"]        = kindName(mKind);
    sd["level"]       = mLevel;
    sd["enabled"]     = mEnabled;
    sd["brightness"]  = mBrightness;
    sd["tintColor"]   = colorToHex(mTint);
    sd["tintAmount"]  = mTintAmount;
    sd["moveSpeed"]   = mMoveSpeed;
    sd["density"]     = mDensity;
    sd["velocity"]    = mVelocity;
    sd["size"]        = mSize;
    sd["sound"]       = mSound;
    sd["volume"]      = mVolume;
    sd["soundPreset"] = mSoundPreset;
    sd["soundLayer"]  = mSoundLayer;
    return sd;
}

bool WolfWeatherProfile::operator==(const WolfWeatherProfile& o) const
{
    return mKind == o.mKind && mLevel == o.mLevel && mEnabled == o.mEnabled
        && mBrightness == o.mBrightness && mTintAmount == o.mTintAmount
        && colorToHex(mTint) == colorToHex(o.mTint)
        && mMoveSpeed == o.mMoveSpeed && mDensity == o.mDensity
        && mVelocity == o.mVelocity && mSize == o.mSize
        && mSound == o.mSound && mVolume == o.mVolume
        && mSoundPreset == o.mSoundPreset && mSoundLayer == o.mSoundLayer;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// presets — the same eight as weather_profile.js PRESETS, field for field
// ═══════════════════════════════════════════════════════════════════════════════════════════

// static
const std::vector<WolfWeatherProfile::Preset>& WolfWeatherProfile::presets()
{
    static const std::vector<Preset> sPresets = {
        { "clear",        "Clear Skies"   },
        { "drizzle",      "Light Drizzle" },
        { "steady",       "Steady Rain"   },
        { "downpour",     "Heavy Rain"    },
        { "thunderstorm", "Thunderstorm"  },
        { "lightsnow",    "Light Snow"    },
        { "snowfall",     "Snowfall"      },
        { "blizzard",     "Blizzard"      },
    };
    return sPresets;
}

// static
const std::vector<WolfWeatherProfile::Preset>& WolfWeatherProfile::soundPresets()
{
    static const std::vector<Preset> sList = {
        { "none",    "No ambience"   },
        { "thunder", "Thunderstorms" },
        { "steady",  "Steady Rain"   },
        { "wind",    "Wind"          },
        { "distant", "Distant Storm" },
    };
    return sList;
}

// static
const std::vector<WolfWeatherProfile::Preset>& WolfWeatherProfile::soundLayers()
{
    static const std::vector<Preset> sList = {
        { "none",   "Silent"       },
        { "soft",   "Soft Patter"  },
        { "heavy",  "Heavy Patter" },
        { "leaves", "On Leaves"    },
        { "water",  "On Water"     },
    };
    return sList;
}

// static
WolfWeatherProfile WolfWeatherProfile::applyPreset(const WolfWeatherProfile& base,
                                                   const std::string& id)
{
    WolfWeatherProfile p = base;
    auto set = [&p](Kind kind, S32 level, S32 bright, const char* hex, S32 tintAmt, S32 move,
                    S32 density, S32 velocity, S32 size, bool sound, S32 vol,
                    const char* amb, const char* near_layer)
    {
        p.mKind = kind; p.mLevel = level; p.mEnabled = true;
        p.mBrightness = bright; p.mTint = colorFromHex(hex, neutralColor(kind));
        p.mTintAmount = tintAmt; p.mMoveSpeed = move;
        p.mDensity = density; p.mVelocity = velocity; p.mSize = size;
        p.mSound = sound; p.mVolume = vol;
        p.mSoundPreset = amb; p.mSoundLayer = near_layer;
    };

    if (id == "clear")
    {
        p.mKind = CLEAR; p.mLevel = 2; p.mEnabled = true; p.mSound = false;
    }
    else if (id == "drizzle")      set(RAIN, 1,  70, "#aaccff", 100,  40,  90,  85,  90, true, 40, "steady",  "soft");
    else if (id == "steady")       set(RAIN, 2,  75, "#aaccff", 100,  60, 100, 100, 100, true, 60, "steady",  "soft");
    else if (id == "downpour")     set(RAIN, 3,  65, "#9fc2f0", 100, 110, 130, 120, 115, true, 75, "steady",  "heavy");
    else if (id == "thunderstorm") set(RAIN, 4,  55, "#8fb4e6", 100, 180, 170, 140, 120, true, 85, "thunder", "heavy");
    else if (id == "lightsnow")    set(SNOW, 1,  90, "#ffffff", 100,  50,  90,  80, 100, true, 30, "wind",    "none");
    else if (id == "snowfall")     set(SNOW, 2,  95, "#ffffff", 100,  70, 110, 100, 110, true, 40, "wind",    "none");
    else if (id == "blizzard")     set(SNOW, 4, 100, "#eef4ff", 100, 260, 180, 160, 120, true, 80, "wind",    "none");
    else return base;

    p.clampAll();
    return p;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// the prim-description form — what a script writes and a builder can type
// ═══════════════════════════════════════════════════════════════════════════════════════════

namespace
{
    /// The integer after "key=" in a description, or `dflt` when the key is absent or unreadable.
    S32 desc_int(const std::string& s, const char* key, S32 dflt)
    {
        const std::string k = std::string(key) + "=";
        const size_t at = s.find(k);
        if (at == std::string::npos) return dflt;
        size_t i = at + k.size();
        bool neg = false;
        if (i < s.size() && s[i] == '-') { neg = true; ++i; }
        if (i >= s.size() || s[i] < '0' || s[i] > '9') return dflt;
        S32 v = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); ++i; }
        return neg ? -v : v;
    }

    /// The word after "key=", up to the next space, or "" when absent.
    std::string desc_word(const std::string& s, const char* key)
    {
        const std::string k = std::string(key) + "=";
        const size_t at = s.find(k);
        if (at == std::string::npos) return std::string();
        const size_t start = at + k.size();
        size_t end = s.find(' ', start);
        if (end == std::string::npos) end = s.size();
        return s.substr(start, end - start);
    }
}

bool WolfWeatherProfile::fromDescription(const std::string& lower)
{
    if (lower.empty()) return false;
    const size_t clear_at = lower.find("wolfclear");
    const size_t rain_at  = lower.find("wolfrain");
    const size_t snow_at  = lower.find("wolfsnow");
    if (clear_at == std::string::npos && rain_at == std::string::npos && snow_at == std::string::npos)
    {
        return false;
    }

    // "wolfclear" WINS over the other two. It is the only keyword that expresses suppression —
    // a covered market, a cave, the inside of a build — and someone who puts it in a parcel is
    // deliberately overriding whatever else is there. Between rain and snow, rain wins, which is
    // the rule this has had since the keywords shipped.
    size_t keyword_end;
    if (clear_at != std::string::npos)
    {
        mKind = CLEAR;
        keyword_end = clear_at + 9;         // strlen("wolfclear")
    }
    else if (rain_at != std::string::npos)
    {
        mKind = RAIN;
        keyword_end = rain_at + 8;          // strlen("wolfrain")
    }
    else
    {
        mKind = SNOW;
        keyword_end = snow_at + 8;          // strlen("wolfsnow")
    }

    // "wolfrain3" -> 3; "wolfrain" alone -> 2 (moderate). A space is allowed between.
    if (mKind != CLEAR)
    {
        size_t i = keyword_end;
        while (i < lower.size() && lower[i] == ' ') ++i;
        mLevel = (i < lower.size() && lower[i] >= '1' && lower[i] <= '4') ? (lower[i] - '0') : 2;
    }

    mBrightness = desc_int(lower, "bright", mBrightness);
    mTintAmount = desc_int(lower, "tint",   mTintAmount);
    mMoveSpeed  = desc_int(lower, "move",   mMoveSpeed);
    mDensity    = desc_int(lower, "dens",   mDensity);
    mVelocity   = desc_int(lower, "vel",    mVelocity);
    mSize       = desc_int(lower, "size",   mSize);
    mVolume     = desc_int(lower, "vol",    mVolume);

    const std::string col = desc_word(lower, "col");
    if (!col.empty()) mTint = colorFromHex(col, mTint);
    const std::string amb = desc_word(lower, "amb");
    if (!amb.empty()) mSoundPreset = amb;
    const std::string nr = desc_word(lower, "near");
    if (!nr.empty()) mSoundLayer = nr;
    const std::string snd = desc_word(lower, "snd");
    if (snd == "on") mSound = true; else if (snd == "off") mSound = false;

    mEnabled = true;
    // Clamped, not refused: a description is somewhere people type words, not an API call.
    clampAll();
    return true;
}
