#include <cassert>
#include <limits>
#include <set>
#include "../wolfwavebrush.h"
int main()
{
    auto cells = [](double ax, double ay, double bx, double by, int diameter) {
        std::set<int> result;
        WolfWaveBrush::visit(16, 16, 16, diameter, ax, ay, bx, by,
            [&](int x, int y) { assert(x >= 0 && x < 16 && y >= 0 && y < 16); result.insert(y * 16 + x); });
        return result;
    };
    assert(cells(8, 8, 8, 8, 1) == std::set<int>{0});
    assert(cells(16, 16, 16, 16, 1) == (std::set<int>{0, 1, 16, 17}));
    auto stroke = cells(8, 8, 248, 8, 1);
    for (int x = 0; x < 16; ++x) assert(stroke.count(x));
    assert(stroke.size() == 16);
    assert(cells(8, 8, 248, 248, 1) == cells(248, 248, 8, 8, 1));
    assert(cells(0, 0, 0, 0, 8).size() > 4);
    assert(cells(-1, 8, 8, 8, 1).empty());
    assert(cells(8, 8, 256, 8, 1).empty());
    assert(cells(std::numeric_limits<double>::quiet_NaN(), 8, 8, 8, 1).empty());
    assert(cells(8, 8, 8, 8, 0).empty());
    assert(cells(8, 8, 8, 8, 9).empty());
    // 2026-09-26: the same dab paints the same 16 m cells on a 256 m region and on Ireland
    // (460,800 m, 28,800 cells an edge), and far corners of the huge grid work.
    auto huge = [](double ax, double ay, double bx, double by, int diameter) {
        std::set<long long> result;
        WolfWaveBrush::visit(28800, 28800, 16, diameter, ax, ay, bx, by,
            [&](int x, int y) { result.insert((long long)y * 28800 + x); });
        return result;
    };
    for (int d = 1; d <= 8; ++d)
    {
        const auto small = cells(100, 120, 140, 90, d);
        std::set<long long> mapped;
        for (int k : small) mapped.insert((long long)(k / 16) * 28800 + k % 16);
        assert(huge(100, 120, 140, 90, d) == mapped);
    }
    assert(huge(460792, 460792, 460792, 460792, 1) == (std::set<long long>{28799LL * 28800 + 28799}));
    assert(huge(460000, 5, 460000, 5, 8).size() > 4);
}
