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
}
