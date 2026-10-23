// [WOLF TERRAFORM 2026-10-23] Off-line checks of wolfterraformmath.h (the pattern of wolfwavebrush_test.cpp: plain asserts, no
// viewer libraries). Build and run:
//   g++ -std=c++17 -O1 -Wall -Wextra -o /tmp/wtm indra/newview/tests/wolfterraformmath_test.cpp && /tmp/wtm
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include "../wolfterraformmath.h"
#include "../wolflayerdata.h"

using namespace WolfTerraformMath;

namespace
{
    // ---- the Cave Builder's merge and message as they were before the move (wolfcave.cpp WolfCaves::shape, git HEAD
    // 5c44e59bbf, lines 1034-1061 and 1071-1088), copied verbatim but for the LL names, as the reference -----------------------
    struct OldP { int x, y; float z; };
    struct OldRun { int x, y0, y1; float z; };
    template <typename T> T llmax(T a, T b) { return (a > b) ? a : b; }
    template <typename T> T llmin(T a, T b) { return (a < b) ? a : b; }
    std::vector<OldRun> oldRuns(const std::vector<OldP>& list, bool to_target)
    {
        constexpr float PUT_BACK_BAND_M = 0.04f;
        std::map<int, std::vector<OldP>> cols;
        for (const OldP& p : list) cols[p.x].push_back(p);
        struct Run { int x, y0, y1; float z; };
        std::vector<Run> runs;
        for (auto& kv : cols)
        {
            std::vector<OldP>& col = kv.second;
            std::sort(col.begin(), col.end(), [](const OldP& a, const OldP& b) { return a.y < b.y; });
            size_t s = 0;
            float lo = col.empty() ? 0.f : col[0].z, hi = lo;
            for (size_t i = 1; i <= col.size(); ++i)
            {
                const bool same = i < col.size() && (to_target ? fabsf(col[i].z - col[s].z) < 0.005f
                                                               : llmax(hi, col[i].z) - llmin(lo, col[i].z) <= PUT_BACK_BAND_M);
                if (i < col.size() && col[i].y == col[i - 1].y + 1 && same)
                {
                    lo = llmin(lo, col[i].z);
                    hi = llmax(hi, col[i].z);
                    continue;
                }
                runs.push_back(Run{ kv.first, col[s].y, col[i - 1].y, to_target ? col[s].z : (lo + hi) * 0.5f });
                s = i;
                if (i < col.size()) lo = hi = col[i].z;
            }
        }
        std::vector<OldRun> out;
        for (const Run& r : runs) out.push_back(OldRun{ r.x, r.y0, r.y1, r.z });
        return out;
    }
    // the old message's ParcelData and Height (msg->addF32Fast West / South / East / North, Height = r.z)
    Flood oldFlood(const OldRun& r) { return Flood{ (float)r.x, (float)r.y0, (float)r.x + 0.5f, (float)r.y1 + 0.5f, r.z }; }

    bool same(float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; }   // bit for bit

    void caveUnchanged()
    {
        std::mt19937 rng(20261023);
        for (int round = 0; round < 400; ++round)
        {
            // a cave's targets: a patch of cells with floors in steps (digging) or rough ground (putting back), with gaps
            std::vector<OldP> pts;
            std::vector<Point> now;
            std::uniform_int_distribution<int> xs(0, 40), ys(0, 40), keep(0, 9), step(0, 3);
            std::uniform_real_distribution<float> rough(20.f, 24.f);
            const bool dig = round % 2 == 0;
            const float floor_z = 18.f + (float)step(rng) * 0.25f;
            for (int x = xs(rng) % 10; x < 30; ++x)
                for (int y = ys(rng) % 10; y < 30; ++y)
                {
                    if (keep(rng) == 0) continue;   // a hole in the list
                    const float z = dig ? (keep(rng) < 2 ? floor_z + 0.003f * (float)step(rng) : floor_z + 0.5f * (float)step(rng)) : rough(rng);
                    pts.push_back(OldP{ x, y, z });
                    now.push_back(Point{ x, y, z });
                }
            // the same list in a shuffled order (the generator's order is not sorted)
            std::vector<size_t> order(pts.size());
            for (size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::shuffle(order.begin(), order.end(), rng);
            std::vector<OldP> po;
            std::vector<Point> pn;
            for (size_t i : order) { po.push_back(pts[i]); pn.push_back(now[i]); }
            const std::vector<OldRun> a = oldRuns(po, dig);
            const std::vector<Run> b = caveRuns(pn, dig, 0.04f);
            assert(a.size() == b.size());
            for (size_t i = 0; i < a.size(); ++i)
            {
                assert(b[i].x0 == a[i].x && b[i].x1 == a[i].x && b[i].y0 == a[i].y0 && b[i].y1 == a[i].y1 && same(b[i].z, a[i].z));
                const Flood fa = oldFlood(a[i]), fb = floodOf(b[i]);
                assert(same(fa.west, fb.west) && same(fa.south, fb.south) && same(fa.east, fb.east) && same(fa.north, fb.north)
                       && same(fa.height, fb.height));
            }
        }
        std::puts("cave floods: identical to the old writer (400 random caves, dig and put back)");
    }

    Grid hills(int x0, int y0, int w, int h, unsigned seed)
    {
        Grid g(x0, y0, w, h);
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> n(-0.3f, 0.3f);
        for (int y = y0; y < y0 + h; ++y)
            for (int x = x0; x < x0 + w; ++x)
                g.ref(x, y) = 22.f + 6.f * std::sin((float)x * 0.07f) + 4.f * std::cos((float)y * 0.05f) + n(rng);
        return g;
    }

    /** Write `target` over `map` as the region would, through planRuns; every cell lands within band / 2 of its target (or was
     *  already within `skip`), and no other cell changes. Returns how many floods it took. */
    size_t writeAndCheck(Grid& map, const Grid& target, float band, float skip)
    {
        const Grid before = map;
        const std::vector<Run> runs = planRuns(target, map, band, skip);
        for (const Run& r : runs) applyFlood(map, r);
        for (int y = map.y0; y < map.y0 + map.h; ++y)
            for (int x = map.x0; x < map.x0 + map.w; ++x)
            {
                const float t = target.at(x, y), now = map.at(x, y), was = before.at(x, y);
                if (!has(t)) { assert(same(now, was)); continue; }
                const float tc = std::min(std::max(t, MIN_TERRAIN_M), MAX_TERRAIN_M);
                assert(std::fabs(now - tc) <= band * 0.5f + 1e-4f || (same(now, was) && std::fabs(was - tc) <= skip + band * 0.5f + 1e-4f));
            }
        return runs.size();
    }

    void plannerAndUndo()
    {
        Grid map = hills(0, 0, 128, 128, 7);
        const Grid ground = map;
        // Level, hard edge: the whole box is ONE flood (DESIGN.md 3.2 #1: "Box or parcel with no blend: one flood")
        {
            Grid m = map;
            const Grid t = level(m, Box{ 10, 20, 49, 39 }, 21.5f, 0.f, 128);
            assert(t.defined() == 40u * 20u);
            assert(writeAndCheck(m, t, 0.04f, 0.02f) == 1);
        }
        // Undo snapshots: the before-heights written back put the ground back within half the band
        {
            Grid m = map;
            const Grid t = level(m, Box{ 30, 30, 70, 60 }, 15.f, 6.f, 128);
            Grid before(t.x0, t.y0, t.w, t.h);
            for (int y = t.y0; y < t.y0 + t.h; ++y)
                for (int x = t.x0; x < t.x0 + t.w; ++x)
                    if (has(t.at(x, y))) before.ref(x, y) = m.at(x, y);
            writeAndCheck(m, t, 0.04f, 0.02f);
            // the "after" the history keeps for Redo is read at Undo time
            Grid after(t.x0, t.y0, t.w, t.h);
            for (int y = t.y0; y < t.y0 + t.h; ++y)
                for (int x = t.x0; x < t.x0 + t.w; ++x)
                    if (has(before.at(x, y))) after.ref(x, y) = m.at(x, y);
            writeAndCheck(m, before, 0.04f, 0.02f);            // Undo
            for (int y = 0; y < 128; ++y)
                for (int x = 0; x < 128; ++x) assert(std::fabs(m.at(x, y) - ground.at(x, y)) <= 0.02f + 1e-4f);
            writeAndCheck(m, after, 0.04f, 0.02f);             // Redo
            for (int y = t.y0; y < t.y0 + t.h; ++y)
                for (int x = t.x0; x < t.x0 + t.w; ++x)
                    if (has(after.at(x, y))) assert(std::fabs(m.at(x, y) - after.at(x, y)) <= 0.02f + 1e-4f);
        }
        // a ramp along y: rows of one height, joined across the columns into one flood a row
        {
            Grid m = map;
            const RampShape r{ 60.f, 10.f, 18.f, 60.f, 70.f, 30.f, 6.f, 0.f };
            const Grid t = ramp(m, r, 128);
            const size_t n = writeAndCheck(m, t, 0.04f, 0.02f);
            assert(n <= 70);   // about one flood a metre of length, not one a column-run
        }
        // rights / limits: a cell the region refuses keeps its height; the plan leaves untargeted cells alone (writeAndCheck)
        std::puts("planner + undo/redo snapshots: every cell within band/2, untouched cells untouched, hard-edged level = 1 flood");
    }

    void rampGrade()
    {
        assert(std::fabs(grade(0, 0, 10, 30, 40, 15) - 0.1f) < 1e-6f);   // 5 m up over 50 m = 10 %
        assert(grade(5, 5, 0, 5, 5, 9) == 0.f);
        const RampShape r{ 10.f, 10.f, 20.f, 50.f, 10.f, 28.f, 4.f, 3.f };
        float z, d;
        assert(rampAt(r, 30.f, 10.f, z, d) && std::fabs(z - 24.f) < 1e-4f && d == 0.f);   // half way: half the rise
        assert(rampAt(r, 30.f, 12.f, z, d) && d == 0.f);                                   // on the strip (half width 2)
        assert(rampAt(r, 30.f, 13.f, z, d) && std::fabs(d - 1.f) < 1e-4f);                // on the shoulder
        assert(!rampAt(r, 30.f, 16.f, z, d));                                              // past it
        assert(rampAt(r, 8.f, 10.f, z, d) && std::fabs(z - 20.f) < 1e-4f && std::fabs(d - 2.f) < 1e-4f);   // beyond end A
        // across the strip the height is the same (no camber); the shoulder eases to the ground
        const Grid ground = hills(0, 0, 64, 64, 3);
        const Grid t = ramp(ground, r, 64);
        assert(std::fabs(t.at(30, 9) - t.at(30, 11)) < 1e-4f);
        const float sh = t.at(30, 13), g = ground.at(30, 13);
        assert((sh - g) * (24.f - g) >= 0.f && std::fabs(sh - g) <= std::fabs(24.f - g) + 1e-4f);
        assert(!has(t.at(30, 20)));
        std::puts("ramp: grade, strip, shoulders, ends");
    }

    void terraceSteps()
    {
        for (float s : { 0.f, 0.3f, 0.6f, 0.9f, 1.f })
        {
            float prev = -1e9f;
            for (float g = -5.f; g < 40.f; g += 0.013f)
            {
                const float t = terraced(g, 2.5f, s);
                assert(t <= g + 1e-4f);              // cuts only
                assert(t >= prev - 1e-4f);           // never turns a slope round
                assert(g - t < 2.5f + 1e-4f);        // never more than a step down
                prev = t;
            }
        }
        assert(std::fabs(terraced(7.4f, 2.5f, 1.f) - 5.f) < 1e-5f);    // crisp: down to its shelf
        assert(std::fabs(terraced(7.4f, 2.5f, 0.f) - 7.4f) < 1e-5f);   // sharpness 0 leaves it
        assert(std::fabs(terraced(-1.f, 2.f, 1.f) + 2.f) < 1e-5f);     // below 0 m too
        std::puts("terrace: cuts only, monotone, crisp shelves on the step");
    }

    void roadBanking()
    {
        const Grid ground = hills(0, 0, 160, 160, 11);
        const RouteLine line({ 20.f, 20.f, 80.f, 40.f, 140.f, 120.f });
        assert(line.usable());
        Profile gp;
        assert(groundProfile(ground, line, gp));
        const Profile p = roadProfile(gp, 6.f, true, 0.08f);
        assert(steepest(p) <= 0.08f + 1e-4f);
        assert(p.z.size() == gp.z.size());
        const Profile lv = roadProfile(gp, 6.f, false, 0.08f);
        assert(std::fabs(lv.z.front() - gp.z.front()) < 1e-5f && std::fabs(lv.z.back() - gp.z.back()) < 1e-5f);
        for (size_t i = 2; i < lv.z.size(); ++i)
            assert(std::fabs((lv.z[i] - lv.z[i - 1]) - (lv.z[1] - lv.z[0])) < 1e-3f);   // one even grade
        const Grid t = road(ground, line, p, 6.f, 4.f, 160);
        // flat across: every cell on the strip has the profile's height at its own place along the line
        int on_strip = 0, on_bank = 0;
        for (int y = t.y0; y < t.y0 + t.h; ++y)
            for (int x = t.x0; x < t.x0 + t.w; ++x)
            {
                const float v = t.at(x, y);
                float d, s;
                line.nearest((float)x, (float)y, d, s);
                if (d > 7.f) { assert(!has(v)); continue; }
                assert(has(v));
                const float z = p.at(s), g = ground.at(x, y);
                if (d <= 3.f) { assert(std::fabs(v - z) < 1e-4f); ++on_strip; }
                else { assert(std::fabs(v - g) <= std::fabs(z - g) + 1e-4f && (v - g) * (z - g) >= -1e-6f); ++on_bank; }
            }
        assert(on_strip > 500 && on_bank > 500);
        std::puts("road: grade limited, even grade, flat across, banks eased to the ground");
    }

    void riverProfile()
    {
        const Grid ground = hills(0, 0, 160, 160, 5);
        const RouteLine line({ 10.f, 80.f, 70.f, 60.f, 150.f, 100.f });
        Profile gp;
        assert(groundProfile(ground, line, gp));
        const Profile bed = riverBed(gp, 5.f, 1.5f, nan());
        for (size_t i = 1; i < bed.z.size(); ++i) assert(bed.z[i] <= bed.z[i - 1] + 1e-6f);   // never runs uphill
        const Profile smooth = smoothProfile(gp, 5.f);
        for (size_t i = 0; i < bed.z.size(); ++i) assert(bed.z[i] <= smooth.z[i] - 1.5f + 1e-4f);
        const Profile sea = riverBed(gp, 5.f, 1.5f, 12.f);
        assert(sea.z.back() <= 12.f + 1e-4f);
        for (size_t i = 1; i < sea.z.size(); ++i) assert(sea.z[i] <= sea.z[i - 1] + 1e-6f);
        const Grid t = river(ground, line, bed, 5.f, 1.5f, 3.f, 160);
        size_t n = 0;
        for (int y = t.y0; y < t.y0 + t.h; ++y)
            for (int x = t.x0; x < t.x0 + t.w; ++x)
            {
                const float v = t.at(x, y);
                if (!has(v)) continue;
                ++n;
                assert(v <= ground.at(x, y) + 1e-5f);   // a channel only cuts
            }
        assert(n > 1000);
        // the U: the bed in the middle, depth higher at the edge, the ground beyond the banks
        assert(std::fabs(riverAcross(10.f, 2.f, 3.f, 2.f, 0.f, 20.f) - 10.f) < 1e-5f);
        assert(std::fabs(riverAcross(10.f, 2.f, 3.f, 2.f, 3.f, 20.f) - 12.f) < 1e-5f);
        assert(std::fabs(riverAcross(10.f, 2.f, 3.f, 2.f, 5.f, 20.f) - 20.f) < 1e-5f);
        std::puts("river: bed falls downstream, depth under the ground, meets the sea, cuts only, U across");
    }

    void beachShore()
    {
        // a straight slope from the sea (x small) up onto land, water at 20 m
        Grid g(0, 0, 80, 20);
        for (int y = 0; y < 20; ++y)
            for (int x = 0; x < 80; ++x) g.ref(x, y) = 10.f + 0.5f * (float)x;   // 20 m at x = 20
        const Grid sd = shoreDistance(g, 20.f);
        assert(std::fabs(sd.at(30, 10) - 10.5f) < 1e-4f && std::fabs(sd.at(10, 10) + 9.5f) < 1e-4f);   // the line lies between x 19 and 20
        const Grid t = beach(g, Box{ 0, 0, 79, 19 }, 20.f, 0.1f, 2.f, 1.5f, 4.f, 80);
        for (int x = 0; x < 80; ++x)
        {
            const float v = t.at(x, 10), d = sd.at(x, 10);
            if (d >= 0.f && d <= 20.f) assert(std::fabs(v - (20.f + 0.1f * d)) < 1e-4f);
            if (d < 0.f && -d <= 15.f) assert(std::fabs(v - (20.f + 0.1f * d)) < 1e-4f);
            if (d > 24.f || -d > 19.f) assert(!has(v));
            const float z = std::min(std::max(20.f + 0.1f * d, 20.f - 1.5f), 20.f + 2.f), gg = g.at(x, 10);
            if (has(v)) assert(v >= std::min(gg, z) - 1e-4f && v <= std::max(gg, z) + 1e-4f);   // between the plane and the ground
        }
        Grid dry(0, 0, 10, 10, 30.f);
        assert(!has(shoreDistance(dry, 20.f).at(5, 5)));   // no water line: nothing to shape
        std::puts("beach: one slope through the water line, the band, eased back, needs a shore");
    }


    // ---- [2026-10-23 Paul's Terrace] the read-back's tolerance: how exactly heights come back through LayerData ---------------
    float patchErr(const float* h)
    {
        float o[256], m = 0.f;
        WolfLayerData::roundTrip(h, o);
        for (int k = 0; k < 256; ++k) m = std::max(m, std::fabs(o[k] - h[k]));
        return m;
    }
    void layerDataTolerance()
    {
        float h[256], o[256];
        for (int k = 0; k < 256; ++k) h[k] = 37.25f;                      // flat: sent as a DC offset only, exact
        WolfLayerData::roundTrip(h, o);
        for (int k = 0; k < 256; ++k) assert(o[k] == 37.25f);
        for (int k = 0; k < 256; ++k) h[k] = 20.f + 0.1f * (float)(k % 16);   // a gentle ramp: millimetres
        assert(patchErr(h) < 0.01f);
        for (int k = 0; k < 256; ++k) h[k] = 20.f + (float)(k % 16);          // 15 m across one patch: still centimetres
        assert(patchErr(h) < 0.06f);
        // Paul's: terraces 2 m apart (sharpness 0.8) on a 20 % slope. The region holds them exactly, yet they decode up to
        // about 0.19 m off - over the old fixed 0.06 m, so every such cell was called refused.
        for (int k = 0; k < 256; ++k) h[k] = terraced(36.f + 0.2f * (float)(k % 16) + 0.12f * (float)(k / 16), 2.f, 0.8f);
        const float e = patchErr(h);
        assert(e > 0.06f && e < 0.3f);
        std::printf("layerdata: flat exact, ramp < 1 cm, a 2 m terrace on a 20%% slope decodes %.3f m off (the old check allowed 0.06)\n", e);
    }

    void judgeAccuracy()
    {
        std::mt19937 rng(11);
        std::uniform_real_distribution<float> U(0.f, 1.f);
        int took = 0, took_wrong = 0, kept = 0, kept_missed = 0, kept_big = 0, kept_big_missed = 0, old_wrong = 0;
        for (int t = 0; t < 1500; ++t)
        {
            float H0[256], H1[256], D0[256], D1[256], sent[256];
            bool tg[256], refused[256], ok[256];
            const float a = U(rng) * 0.8f, b = U(rng) * 0.8f, base = 10.f + U(rng) * 50.f, wob = U(rng) * 3.f;
            for (int k = 0; k < 256; ++k) H0[k] = base + a * (float)(k % 16) + b * (float)(k / 16) + wob * std::sin((float)(k % 16) * 0.5f + (float)(k / 16) * 0.3f);
            const int x0 = (int)(rng() % 16), x1 = x0 + (int)(rng() % (16 - x0)), y0 = (int)(rng() % 16), y1 = y0 + (int)(rng() % (16 - y0));
            const int kind = (int)(rng() % 3), scen = (int)(rng() % 3), cut = (int)(rng() % 16);
            const float lvl = base + U(rng) * 5.f;
            for (int k = 0; k < 256; ++k)
            {
                const int x = k % 16, y = k / 16;
                tg[k] = x >= x0 && x <= x1 && y >= y0 && y <= y1;
                const float T = kind == 0 ? terraced(H0[k], 2.f, 0.8f) : kind == 1 ? lvl : H0[k] - 1.5f * (float)(x - x0) / (float)(1 + x1 - x0);
                sent[k] = tg[k] ? T : nan();
                // all taken, all kept, or kept on one side of a parcel line
                refused[k] = tg[k] && (scen == 1 || (scen == 2 && x < cut));
                H1[k] = tg[k] && !refused[k] ? T : H0[k];
            }
            WolfLayerData::roundTrip(H0, D0);
            WolfLayerData::roundTrip(H1, D1);
            WolfLayerData::judge(D1, sent, D0, 0.06f, ok);
            for (int k = 0; k < 256; ++k)
            {
                if (!tg[k]) continue;
                const float shortfall = std::fabs(sent[k] - H0[k]);
                if (!refused[k]) { ++took; took_wrong += !ok[k]; old_wrong += std::fabs(D1[k] - sent[k]) > 0.06f; }
                else
                {
                    ++kept; kept_missed += ok[k];
                    if (shortfall > 0.25f) { ++kept_big; kept_big_missed += ok[k]; }
                }
            }
        }
        const double wrong = 100.0 * took_wrong / took, old = 100.0 * old_wrong / took, missed = 100.0 * kept_big_missed / kept_big;
        std::printf("judge: of %d taken cells %.2f%% called not taken (the old 0.06 m test: %.1f%%); of %d kept cells short by over 0.25 m, %.2f%% missed (%.1f%% of all kept)\n",
                    took, wrong, old, kept_big, missed, 100.0 * kept_missed / kept);
        assert(wrong < 1.5 && old > 10.0 && missed < 3.0);
    }

    void beachSmooth()
    {
        // a coast diagonal through the box; land rising on, sea floor falling off
        Grid g(0, 0, 200, 200);
        for (int y = 0; y < 200; ++y)
            for (int x = 0; x < 200; ++x)
                g.ref(x, y) = 20.f + 0.15f * ((float)(x - 100) * 0.8f + (float)(y - 100) * 0.6f) + 0.3f * std::sin((float)x * 0.9f + (float)y * 0.4f);
        const float slope = 0.1f, edge = 4.f;
        const Grid t = beach(g, Box{ 70, 55, 127, 144 }, 20.f, slope, 2.f, 2.f, edge, 200);
        // the beach's own plane before any easing (what the eases join to the ground), from the same shore distance
        Grid box(70, 55, 58, 90);
        for (int y = 55; y <= 144; ++y) for (int x = 70; x <= 127; ++x) box.ref(x, y) = g.at(x, y);
        const Grid sd = shoreDistance(box, 20.f);
        auto plane = [&](int x, int y) { const float d = sd.at(x, y); return has(d) ? std::min(std::max(20.f + slope * d, 18.f), 22.f) : g.at(x, y); };
        Grid fin = g;   // the finished ground
        for (size_t i = 0; i < t.cells(); ++i)
        {
            const int x = t.x0 + (int)(i % (size_t)t.w), y = t.y0 + (int)(i / (size_t)t.w);
            if (has(t.v[i])) fin.ref(x, y) = t.v[i];
        }
        // no step between neighbours steeper than the beach's own slope, the ground's own step, and the ease's steepest
        // (a cosine fall over `edge` between the two heights it joins: pi / (2 edge) per metre of the difference)
        float worst = 0.f;
        for (int y = 40; y < 160; ++y)
            for (int x = 55; x < 145; ++x)
                for (int d = 0; d < 2; ++d)
                {
                    const int nx = x + (d == 0), ny = y + (d == 1);
                    const float step = std::fabs(fin.at(x, y) - fin.at(nx, ny)), ground = std::fabs(g.at(x, y) - g.at(nx, ny));
                    // fin = g + w (z - g): its step is at most the ground's, plus w times the plane's against the ground's, plus the
                    // weight's own step (a cosine ease over `edge`: at most pi / (2 edge) a metre, two eases multiplied) times z - g
                    const float joined = std::max(std::fabs(plane(x, y) - g.at(x, y)), std::fabs(plane(nx, ny) - g.at(nx, ny)));
                    const float bound = ground + (slope + ground) + 2.f * 3.14159265f / (2.f * edge) * joined + 1e-3f;
                    worst = std::max(worst, step / bound);
                    if (step > bound) std::printf("  step %.3f > %.3f at %d,%d d%d: fin %.3f/%.3f g %.3f/%.3f\n", step, bound, x, y, d, fin.at(x, y), fin.at(nx, ny), g.at(x, y), g.at(nx, ny));
                    assert(step <= bound);
                }
        // no spikes: no finished cell stands out from all eight neighbours by more than the ground already did there
        for (int y = 41; y < 159; ++y)
            for (int x = 56; x < 144; ++x)
            {
                float lo = 1e9f, hi = -1e9f, glo = 1e9f, ghi = -1e9f;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        if (dx || dy) { lo = std::min(lo, fin.at(x + dx, y + dy)); hi = std::max(hi, fin.at(x + dx, y + dy));
                                        glo = std::min(glo, g.at(x + dx, y + dy)); ghi = std::max(ghi, g.at(x + dx, y + dy)); }
                const float out = std::max(fin.at(x, y) - hi, lo - fin.at(x, y)), gout = std::max(g.at(x, y) - ghi, glo - g.at(x, y));
                assert(out <= std::max(0.f, gout) + 0.05f);
            }
        // and at the box's own side the beach is (almost) the ground: no wall where it meets the land outside
        for (int y = 55; y <= 144; ++y) assert(std::fabs(fin.at(70, y) - g.at(69, y)) <= std::fabs(g.at(70, y) - g.at(69, y)) + 0.6f);
        std::printf("beach: smooth into the ground on every side (worst step %.0f%% of its bound), no spikes\n", worst * 100.f);
    }

    void floodCounts()
    {
        Grid coast(0, 0, 200, 200), flat(0, 0, 200, 200), slope(0, 0, 200, 200);
        for (int y = 0; y < 200; ++y)
            for (int x = 0; x < 200; ++x)
            {
                coast.ref(x, y) = 20.f + 0.15f * ((float)(x - 100) * 0.8f + (float)(y - 100) * 0.6f) + 0.3f * std::sin((float)x * 0.9f + (float)y * 0.4f);
                flat.ref(x, y) = 22.f + 6.f * std::sin((float)x * 0.07f) + 4.f * std::cos((float)y * 0.05f) + 0.3f * std::sin((float)x * 1.3f + (float)y * 0.9f);
                slope.ref(x, y) = 30.f + 0.2f * (float)x + 0.12f * (float)y;
            }
        struct Case { const char* name; Grid g, t; };
        const Case cases[] = {
            { "beach 58 x 90 m, 10 %", coast, beach(coast, Box{ 70, 55, 127, 144 }, 20.f, 0.1f, 2.f, 2.f, 4.f, 200) },
            { "terrace 25 x 35 m, 2 m", slope, terrace(slope, Box{ 50, 50, 74, 84 }, 2.f, 0.8f, 2.f, 200) },
            { "ramp 60 x 6 m", flat, ramp(flat, RampShape{ 40, 100, 18, 100, 100, 30, 6, 3 }, 200) },
            { "level box 40 x 20 m", flat, level(flat, Box{ 60, 60, 99, 79 }, 23.f, 0.f, 200) },
            { "level box, 4 m soft edge", flat, level(flat, Box{ 60, 60, 99, 79 }, 23.f, 4.f, 200) } };
        for (const Case& c : cases)
        {
            const size_t before = planRuns(c.t, c.g, 0.04f, 0.02f).size(), after = planRuns(c.t, c.g, 0.1f, 0.05f).size();
            assert(after <= before);
            Grid m = c.g;
            writeAndCheck(m, c.t, 0.1f, 0.05f);   // still within half the band (and nothing else touched)
            std::printf("floods: %-26s %5zu -> %5zu\n", c.name, before, after);
        }
    }


    // [2026-10-23 Paul: "some of our regions are massive"] Ireland is one 460,800 m region (paged terrain; the viewer still holds
    // it at one sample a metre in 16 m patches, llviewerregion.cpp:707-726, made as they arrive). Coordinates there need care:
    // a float has a 1/64 m step at 200,000 m, still fine for the .5 a flood adds to its east and north edges.
    void bigRegion()
    {
        const int W = 460800, x0 = 200000, y0 = 310000;
        Grid g(x0 - 64, y0 - 64, 256, 256);
        for (int y = g.y0; y < g.y0 + g.h; ++y)
            for (int x = g.x0; x < g.x0 + g.w; ++x)
                g.ref(x, y) = 40.f + 0.1f * (float)(x - x0) + 3.f * std::sin((float)(y - y0) * 0.05f);
        // every tool's target lands where it was asked, inside the region
        const Grid lv = level(g, Box{ x0, y0, x0 + 39, y0 + 19 }, 45.f, 3.f, W);
        assert(lv.x0 == x0 - 3 && lv.y0 == y0 - 3 && std::fabs(lv.at(x0 + 10, y0 + 10) - 45.f) < 1e-4f);
        const Grid rp = ramp(g, RampShape{ (float)x0, (float)y0 + 50.f, 40.f, (float)x0 + 60.f, (float)y0 + 50.f, 46.f, 6.f, 3.f }, W);
        assert(std::fabs(rp.at(x0 + 30, y0 + 50) - 43.f) < 1e-3f);
        const RouteLine line({ (float)x0 + 5.f, (float)y0 + 100.f, (float)x0 + 80.f, (float)y0 + 140.f, (float)x0 + 150.f, (float)y0 + 120.f });
        Profile gp;
        assert(line.usable() && groundProfile(g, line, gp));
        const Grid rd = road(g, line, roadProfile(gp, 6.f, true, 0.25f), 6.f, 4.f, W);
        float d, s;
        line.nearest((float)x0 + 80.f, (float)y0 + 140.f, d, s);
        assert(d < 1e-2f && has(rd.at(x0 + 80, y0 + 140)));
        // the floods: the region floods (int)west..(int)east (TerrainModule.cs:1855-1860), so east = x1 + 0.5 must stay exact
        Grid m = g;
        writeAndCheck(m, lv, 0.1f, 0.05f);
        writeAndCheck(m, rp, 0.1f, 0.05f);
        writeAndCheck(m, rd, 0.1f, 0.05f);
        for (const Run& r : planRuns(rd, g, 0.1f, 0.05f))
        {
            const Flood f = floodOf(r);
            assert((int)f.west == r.x0 && (int)f.east == r.x1 && (int)f.south == r.y0 && (int)f.north == r.y1);
            assert(f.east - (float)r.x1 == 0.5f && f.north - (float)r.y1 == 0.5f);
        }
        // the region's far corner too
        const Flood far = floodOf(Run{ W - 1, W - 1, W - 1, W - 1, 12.f });
        assert((int)far.east == W - 1 && far.east - (float)(W - 1) == 0.5f);
        // a box past the region's edge is clipped to it
        const Grid edge = level(g, Box{ W - 10, 5, W + 30, 20 }, 30.f, 0.f, W);
        assert(edge.x0 + edge.w == W);
        // [Paul's hard rule] no job, Undo or preset may ever cover the whole region: a grid is the job's rectangle only, and
        // anything wider than MAX_GRID_SIDE (or the whole of Ireland) gets no grid at all - nothing region-sized is allocated
        const Grid mid = level(g, Box{ 230000, 230000, 230499, 230499 }, 30.f, 4.f, W);   // 500 m in the middle
        assert(mid.w == 508 && mid.h == 508 && mid.cells() == 508u * 508u);
        Grid corner_ground(W - 300, W - 300, 300, 300, 20.f);
        const Grid corner = level(corner_ground, Box{ W - 256, W - 256, W - 1, W - 1 }, 30.f, 4.f, W);   // the far corner, clipped
        assert(corner.x0 == W - 260 && corner.x0 + corner.w == W && corner.y0 + corner.h == W && corner.cells() == 260u * 260u);
        assert(has(corner.at(W - 1, W - 1)));
        const Grid whole = level(g, Box{ 0, 0, W - 1, W - 1 }, 30.f, 0.f, W);
        assert(whole.cells() == 0 && whole.v.capacity() == 0);
        assert(areaGrid(Box{ 0, 0, W - 1, W - 1 }, 0, W).cells() == 0);
        assert(areaGrid(Box{ 1000, 1000, 1000 + MAX_GRID_SIDE, 1001 }, 0, W).cells() == 0);    // one past the limit
        assert(areaGrid(Box{ 1000, 1000, 999 + MAX_GRID_SIDE, 1001 }, 0, W).cells() == (size_t)MAX_GRID_SIDE * 2u);
        assert(smooth(g, Box{ 0, 0, W - 1, W - 1 }, 8, 1.f, W).cells() == 0);
        assert(beach(g, Box{ 0, 0, W - 1, W - 1 }, 20.f, 0.1f, 2.f, 2.f, 4.f, W).cells() == 0);
        std::puts("big region: tools, floods and the edge exact at x 200,000 m on a 460,800 m region; jobs in the middle and at the far "
                  "corner allocate only their rectangle; a whole-region box allocates nothing");
    }

    void smoothWide()
    {
        Grid g = hills(0, 0, 100, 100, 9);
        g.ref(50, 50) = 60.f;   // a spike
        g.ref(50, 51) = -20.f;  // and a pit below 0 m (the region's own smooth stops at 0 m)
        const Grid t = smooth(g, Box{ 30, 30, 70, 70 }, 12, 1.f, 100);
        assert(t.at(50, 50) < 30.f && t.at(50, 51) > 5.f);
        assert(!has(t.at(29, 50)) && has(t.at(30, 50)));
        // near the box's edge it barely changes (eased in, no step at the edge)
        assert(std::fabs(t.at(30, 50) - g.at(30, 50)) < std::fabs(t.at(45, 50) - g.at(45, 50)) + 0.5f);
        std::puts("smooth: wide radius, below 0 m, eased at the box edge");
    }
}

int main()
{
    caveUnchanged();
    plannerAndUndo();
    rampGrade();
    terraceSteps();
    roadBanking();
    riverProfile();
    beachShore();
    smoothWide();
    layerDataTolerance();
    judgeAccuracy();
    beachSmooth();
    floodCounts();
    bigRegion();
    std::puts("wolfterraformmath: all checks passed");
    return 0;
}
