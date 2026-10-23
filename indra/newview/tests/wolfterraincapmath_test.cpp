// [WOLF TERRAFORM 2026-10-23 Phase 3] Off-line checks of wolfterraincapmath.h (the pattern of wolfterraformmath_test.cpp: plain
// asserts, no viewer libraries): how the terrain writer's points become WolfTerraform requests, and the reading of the answer.
// Build and run:
//   g++ -std=c++17 -O1 -Wall -Wextra -o /tmp/wtc indra/newview/tests/wolfterraincapmath_test.cpp && /tmp/wtc
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include "../wolfterraincapmath.h"

using namespace WolfTerraformMath;
using namespace WolfTerrainCapMath;

namespace
{
    // Source: WolfTerraformModule.Apply - BinaryPrimitives.ReadSingleLittleEndian(raw.AsSpan(i * 4, 4)), written out by hand
    float readLE(const std::vector<uint8_t>& b, size_t i)
    {
        uint32_t u = (uint32_t)b[i * 4] | ((uint32_t)b[i * 4 + 1] << 8) | ((uint32_t)b[i * 4 + 2] << 16) | ((uint32_t)b[i * 4 + 3] << 24);
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    }
    // Source: TerrainModule.WolfTerraform.cs WolfTiles - the region's own count, its cap is 4 per request
    int tiles(int start, int len) { return (start + len - 1) / 256 - start / 256 + 1; }
}

int main()
{
    // 1. the 58 x 90 m beach (DESIGN brief: 3,665 floods by the old route) at a block corner: 4 requests, each inside one block
    {
        std::vector<Point> pts;
        for (int y = 220; y < 310; ++y)
            for (int x = 230; x < 288; ++x) pts.push_back(Point{ x, y, 20.f + 0.01f * (float)(x - 230) - 0.003f * (float)(y - 220) });
        std::vector<Batch> b = makeBatches(pts);
        assert(b.size() == 4);
        size_t n = 0;
        for (const Batch& q : b)
        {
            assert(q.w >= 1 && q.h >= 1 && q.w <= BLOCK && q.h <= BLOCK);
            assert(tiles(q.x0, q.w) * tiles(q.y0, q.h) == 1);       // one terrain tile each, never more
            assert(q.heights.size() == (size_t)q.w * (size_t)q.h);
            n += q.pts.size();
        }
        assert(n == pts.size());
        // the block south-west of (256, 256): x 230..255, y 220..255, tight to the points
        assert(b[0].x0 == 230 && b[0].y0 == 220 && b[0].w == 26 && b[0].h == 36);
        std::printf("PASS the 58 x 90 m beach: 4 requests (was 3,665 floods), each one tile, every point in exactly one\n");
    }

    // 2. heights land in the right cell, row by row from y0; cells without a point are NaN (left alone by the region)
    {
        std::vector<Point> pts = { { 10, 20, 1.5f }, { 12, 20, -3.25f }, { 10, 22, 4000.f }, { 11, 21, -100.f } };
        std::vector<Batch> b = makeBatches(pts);
        assert(b.size() == 1);
        const Batch& q = b[0];
        assert(q.x0 == 10 && q.y0 == 20 && q.w == 3 && q.h == 3);
        assert(q.heights[0] == 1.5f && q.heights[2] == -3.25f && q.heights[6] == 4000.f && q.heights[4] == -100.f);
        assert(std::isnan(q.heights[1]) && std::isnan(q.heights[3]) && std::isnan(q.heights[8]));
        std::vector<uint8_t> enc = encode(q.heights);
        assert(enc.size() == 36);
        for (size_t i = 0; i < q.heights.size(); ++i)
        {
            const float v = readLE(enc, i);
            assert((std::isnan(v) && std::isnan(q.heights[i])) || v == q.heights[i]);
        }
        // exact little-endian bytes of 1.5f = 0x3FC00000
        assert(enc[0] == 0x00 && enc[1] == 0x00 && enc[2] == 0xC0 && enc[3] == 0x3F);
        std::printf("PASS cells row by row from y0, NaN where no point, float32 little-endian bit for bit\n");
    }

    // 3. random points over a 1024 m job: blocks never cross the 256 m grid, and every height round-trips exactly
    {
        std::mt19937 rng(11);
        std::uniform_int_distribution<int> xy(0, 1023);
        std::uniform_real_distribution<float> z(-100.f, 4000.f);
        std::vector<Point> pts;
        std::set<std::pair<int, int>> seen;
        for (int i = 0; i < 20000; ++i)
        {
            Point p{ xy(rng), xy(rng), z(rng) };
            if (seen.insert({ p.x, p.y }).second) pts.push_back(p);
        }
        std::vector<Batch> b = makeBatches(pts);
        assert(b.size() == 16);
        size_t n = 0;
        for (const Batch& q : b)
        {
            assert(q.x0 / BLOCK == (q.x0 + q.w - 1) / BLOCK && q.y0 / BLOCK == (q.y0 + q.h - 1) / BLOCK);
            std::vector<uint8_t> enc = encode(q.heights);
            for (const Point& p : q.pts) assert(readLE(enc, (size_t)(p.y - q.y0) * (size_t)q.w + (size_t)(p.x - q.x0)) == p.z);
            n += q.pts.size();
        }
        assert(n == pts.size());
        std::printf("PASS 20,000 random points over 1024 m: 16 requests on the 256 m grid, every height exact\n");
    }

    // 4. the answer: cells 2 (refused) and 3 (held at the limit) are not set; 0 and 1 are; no cells = all set; a wrong size = none
    {
        std::vector<Point> pts = { { 0, 0, 1.f }, { 1, 0, 2.f }, { 0, 1, 3.f }, { 1, 1, 4.f } };
        Batch q = makeBatches(pts)[0];
        std::vector<uint8_t> codes;
        std::vector<Point> off = notSet(q, { CELL_SET, CELL_RIGHTS, CELL_LIMIT, CELL_LEFT }, codes);
        assert(off.size() == 2 && off[0].x == 1 && off[0].y == 0 && off[1].x == 0 && off[1].y == 1);
        assert(codes.size() == 2 && codes[0] == CELL_RIGHTS && codes[1] == CELL_LIMIT);
        assert(notSet(q, {}, codes).empty() && codes.empty());
        assert(notSet(q, { 0, 0, 0 }, codes).size() == 4 && codes[0] == CELL_RIGHTS);
        std::printf("PASS the answer: refused and held cells come back as not set, with their reason\n");
    }

    // 5. what the region would refuse never goes: negative coordinates and non-finite heights are dropped; empty = no request
    {
        std::vector<Point> pts = { { -1, 5, 1.f }, { 5, -1, 1.f }, { 3, 3, std::nanf("") }, { 4, 4, INFINITY } };
        assert(makeBatches(pts).empty());
        assert(makeBatches({}).empty());
        std::printf("PASS nothing the region would refuse is sent\n");
    }

    // 6. a far corner of a 460,800 m paged region (Ireland): one small request, inside its own tile
    {
        const int SIDE = 1800 * 256;
        std::vector<Point> pts;
        for (int y = SIDE - 10; y < SIDE; ++y)
            for (int x = SIDE - 10; x < SIDE; ++x) pts.push_back(Point{ x, y, 77.25f });
        std::vector<Batch> b = makeBatches(pts);
        assert(b.size() == 1 && b[0].x0 == SIDE - 10 && b[0].w == 10 && b[0].h == 10 && tiles(b[0].x0, b[0].w) == 1);
        std::printf("PASS the far corner of a 460,800 m region: one 10 x 10 request in one tile\n");
    }

    std::printf("ALL PASS\n");
    return 0;
}
