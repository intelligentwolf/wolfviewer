// [WolfViewer 2026-11-24 ticket 655] wolfweatherhaze.h: the same cases as the server's regression
// (firestorm/region-terrain-work/t655-weather-haze-2026-11-24/servertest). g++ -std=c++17 tests/wolfweatherhaze_test.cpp
#include "../wolfweatherhaze.h"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace WolfWeatherHaze;
static float km(float sigma) { return MOR_LN / sigma / 1000.f; }
static bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }
static Weather W(const char* kind, float fog, float precip, float vis, float rh = 96.f)
{
    Weather w; w.kind = kind; w.fog = fog; w.precipitation = precip; w.visibility = vis; w.humidity = rh; return w;
}

int main()
{
    const float clear = (0.2447f + 0.7f) * 0.0001f * 0.8f;
    // dry: the 2026-10-27 limit, unchanged; a clearer EEP keeps its own, a hazier one is held
    assert(!isWet(W("clear", 0.f, 0.f, 30000.f)));
    assert(near(worldSigma(W("clear", 0.f, 0.f, 30000.f), 0.f), clear, 1e-9f));
    assert(near(worldSigma(W("clear", 0.f, 0.f, -1.f), 0.f), clear, 1e-9f));
    assert(!isWet(W("clear", 0.f, 0.f, 6000.f)));
    assert(distanceMultiplier(clear * 0.5f, 0.4f, clear, false) == 0.4f);
    assert(near(distanceMultiplier(clear * 5.f, 4.f, clear, false), 0.8f, 1e-5f));
    // mist: visibility 4,980 m at 96 % RH, nothing falling
    { Weather w = W("clear", 0.f, 0.f, 4980.f); assert(isWet(w)); assert(near(km(worldSigma(w, 0.f)), 4.98f, 0.01f)); }
    // haze, not mist (CAP 746 4.86): the same visibility at 80 % RH (TROMSO 24 Nov 12:52Z) keeps the dry look
    { Weather w = W("clear", 0.f, 0.f, 4980.f, 80.4f); assert(!isWet(w)); assert(near(worldSigma(w, 0.f), clear, 1e-9f)); }
    // rain and snow with no visibility: Gultepe et al. 2010 mean fits
    { const float r[] = { 0.1f, 1.f, 5.f };
      for (float x : r) assert(near(km(worldSigma(W("rain", 0.f, x, -1.f), 0.f)), 9.01f - 4.12f * std::pow(x, 0.176f), 0.01f)); }
    { const float r[] = { 0.1f, 0.5f, 2.f };
      for (float x : r) assert(near(km(worldSigma(W("snow", 0.f, x, -1.f), 0.f)), std::fmax(1.f, 1.10f * std::pow(x, -0.701f)), 0.01f)); }
    // the precipitation floor beside a clearer model visibility; a hazier visibility wins; 1 km floor
    assert(near(km(worldSigma(W("rain", 0.f, 1.f, 20000.f), 0.f)), 4.89f, 0.01f));
    assert(near(km(worldSigma(W("rain", 0.f, 1.f, 2500.f), 0.f)), 2.5f, 0.01f));
    assert(near(km(worldSigma(W("rain", 0.f, 0.5f, 300.f), 0.f)), 1.f, 0.01f));
    // rain counts whatever the humidity; a low model visibility below 95 % RH does not (Narvik 24 Nov: 240 m at 71 %)
    assert(isWet(W("rain", 0.f, 0.5f, -1.f, 60.f)));
    assert(near(km(worldSigma(W("rain", 0.f, 1.f, 240.f, 71.f), 0.f)), 4.89f, 0.01f));
    // a rain symbol with nothing in the hour and a clear visibility: dry
    assert(!isWet(W("rain", 0.f, 0.f, 30000.f)));
    // wet SETS: a clearer EEP is raised, a hazier one lowered, to the weather's
    { const float ws = worldSigma(W("rain", 0.f, 1.f, -1.f), 0.f);
      assert(near(distanceMultiplier(clear, 0.8f, ws, true) * clear / 0.8f, ws, ws * 1e-4f));
      assert(near(distanceMultiplier(ws * 10.f, 8.f, ws, true) * (ws * 10.f) / 8.f, ws, ws * 1e-4f)); }
    // no weather: no limit
    assert(distanceMultiplier(clear, 0.8f, -1.f, true) == 0.8f);
    std::puts("wolfweatherhaze_test: all passed");
    return 0;
}
