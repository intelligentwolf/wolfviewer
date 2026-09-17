#include "llterraingridoffset.h"
#include <cassert>
#include <cstdlib>
#include <iostream>
int main()
{
    for (int width : {256, 4096, 25600, 51200, 102400})
    {
        int stride = width + 1;
        auto count = terrainGridOffset(0, stride, stride);
        assert(count == (static_cast<long long>(width) + 1) * (width + 1));
        assert(terrainGridOffset(width, width, stride) == count - 1);
        float* heights = static_cast<float*>(std::calloc(static_cast<size_t>(count), sizeof(float)));
        assert(heights);
        heights[terrainGridOffset(width, width, stride)] = 91.f;
        heights[terrainGridOffset(203, 117, stride)] = 32.f;
        assert(heights[count - 1] == 91.f);
        assert(heights[terrainGridOffset(203, 117, stride)] == 32.f);
        std::free(heights);
    }
    std::cout << "PASS: terrain allocation and far-corner addressing through 102400 metres\n";
}
