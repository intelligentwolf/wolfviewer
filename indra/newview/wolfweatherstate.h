/**
 * @file wolfweatherstate.h
 * @brief Small, viewer-independent state helpers for parcel weather.
 */

#ifndef WOLF_WEATHER_STATE_H
#define WOLF_WEATHER_STATE_H

#include <cstdint>
#include <map>
#include <string>

// Source: wolfstorm/js/world/wolfweather.js WolfWeather._applyPrecedence() (2026-09-14) and
// parcel-weather-work/plan.md: preview > prim description > parcel > region > personal.
enum class WolfWeatherSource
{
    MENU,
    REGION,
    PARCEL,
    PRIM,
    PREVIEW
};

inline WolfWeatherSource wolfWeatherSource(bool preview, bool prim, bool parcel, bool region)
{
    if (preview) return WolfWeatherSource::PREVIEW;
    if (prim)    return WolfWeatherSource::PRIM;
    if (parcel)  return WolfWeatherSource::PARCEL;
    if (region)  return WolfWeatherSource::REGION;
    return WolfWeatherSource::MENU;
}

// Source: parcel-weather-work/plan.md: prim and parcel layers include complete art-direction
// fields. Region art direction is inherited only by region weather and the personal-menu
// fallback beneath it.
inline bool wolfWeatherUsesRegionArtDirection(WolfWeatherSource source)
{
    return source == WolfWeatherSource::REGION || source == WolfWeatherSource::MENU;
}

// Source: weather.php:212-224 parcel_payload(). `applies` is exactly the current region policy,
// a resolved row, that row enabled, and a non-null profile kind. Keeping the policy operand local
// prevents independently versioned parcel and region replies from defeating one another.
inline bool wolfWeatherParcelApplies(bool allow_parcel, bool have_parcel,
                                     bool enabled, bool have_kind)
{
    return allow_parcel && have_parcel && enabled && have_kind;
}

// Source: wolfstorm/js/world/wolfweather.js WolfWeather._activePreview()/setPreview()
// (2026-09-14). Each editor owns one slot; changing it makes it newest, and clearing one owner
// leaves every other owner's preview.
template<typename T>
class WolfWeatherPreviewRegistry
{
public:
    using owner_t = std::uint64_t;

    void set(owner_t owner, const T& value)
    {
        Entry& entry = mEntries[owner];
        entry.mValue = value;
        entry.mOrder = ++mOrder;
    }

    void clear(owner_t owner) { mEntries.erase(owner); }

    const T* active() const
    {
        const Entry* newest = nullptr;
        for (const auto& pair : mEntries)
        {
            if (!newest || pair.second.mOrder > newest->mOrder) newest = &pair.second;
        }
        return newest ? &newest->mValue : nullptr;
    }

private:
    struct Entry
    {
        T             mValue{};
        std::uint64_t mOrder = 0;
    };

    std::map<owner_t, Entry> mEntries;
    std::uint64_t            mOrder = 0;
};

// Source: LLAgent::changeParcels() (llagent.cpp:1142-1151) emits after the current parcel is
// installed, and LLAgent::setRegion() (llagent.cpp:1178-1315) emits the region crossing first.
// A captured generation belongs to exactly one current target and is invalid after advance().
class WolfWeatherAsyncGate
{
public:
    using generation_t = std::uint64_t;

    generation_t current() const { return mGeneration; }
    void advance() { ++mGeneration; }
    bool accepts(generation_t generation) const { return generation == mGeneration; }

private:
    generation_t mGeneration = 1;
};

// Source: LLView::onVisibilityChange()/isInVisibleChain() (llview.cpp:406,663-700) and
// LLAgent::changeParcels()/setRegion() (llagent.cpp:1142-1151,1178-1315). An asynchronous editor
// result is valid only in the same visible target session and before any newer user edit.
class WolfWeatherEditGate
{
public:
    struct token_t
    {
        std::uint64_t mSession;
        std::uint64_t mRevision;
    };

    token_t capture() const { return {mSession, mRevision}; }
    void changed() { ++mRevision; }
    void newSession() { ++mSession; mRevision = 0; }
    bool accepts(const token_t& token) const
    {
        return token.mSession == mSession && token.mRevision == mRevision;
    }

private:
    std::uint64_t mSession = 1;
    std::uint64_t mRevision = 0;
};

// Source: LLCore coroutine completion ordering in wolfregionweather.cpp. Fetch errors and the
// other save scope share the singleton but must not mutate the durable outcome for this serial.
class WolfWeatherSaveResult
{
public:
    void record(std::uint64_t serial, bool succeeded, const std::string& error)
    {
        mSerial = serial;
        mSucceeded = succeeded;
        mError = error;
    }

    bool completed(std::uint64_t serial) const { return serial != 0 && serial == mSerial; }
    bool succeeded() const { return mSucceeded; }
    const std::string& error() const { return mError; }

private:
    std::uint64_t mSerial = 0;
    bool mSucceeded = false;
    std::string mError;
};

#endif // WOLF_WEATHER_STATE_H
