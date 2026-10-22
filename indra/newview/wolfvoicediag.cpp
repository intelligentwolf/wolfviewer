/**
 * @file wolfvoicediag.cpp
 * @brief WolfViewer voice debug mode (2026-10-22). See wolfvoicediag.h.
 */

#include "llviewerprecompiledheaders.h"

#include "wolfvoicediag.h"

#include <deque>
#include <ctime>

#include "lldate.h"

namespace
{
    const size_t MAX_EVENTS = 60;
    const size_t MAX_EVENT_CHARS = 160;
    bool sEnabled = false;
    std::deque<std::string> sEvents;
}

bool WolfVoiceDiag::enabled()
{
    return sEnabled;
}

void WolfVoiceDiag::setEnabled(bool on)
{
    sEnabled = on;
    if (!on) sEvents.clear();
}

void WolfVoiceDiag::note(const std::string& event)
{
    if (!sEnabled) return;
    // Seconds since the epoch with milliseconds (LLDate::now), UTC, so reports from different machines line up.
    const F64 now = LLDate::now().secondsSinceEpoch();
    const time_t whole = (time_t)now;
    struct tm t;
#if LL_WINDOWS
    gmtime_s(&t, &whole);
#else
    gmtime_r(&whole, &t);
#endif
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d ", t.tm_hour, t.tm_min, t.tm_sec, (int)((now - (F64)whole) * 1000.0));
    std::string line = stamp + event.substr(0, MAX_EVENT_CHARS);
    sEvents.push_back(line);
    while (sEvents.size() > MAX_EVENTS) sEvents.pop_front();
}

LLSD WolfVoiceDiag::events()
{
    LLSD out = LLSD::emptyArray();
    for (const std::string& e : sEvents) out.append(e);
    return out;
}

void WolfVoiceDiag::clear()
{
    sEvents.clear();
}
