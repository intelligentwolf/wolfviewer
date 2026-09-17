#ifndef LL_LLTERRAINGRIDOFFSET_H
#define LL_LLTERRAINGRIDOFFSET_H
#include <cstddef>
// Source: LLSurface and LLViewerLayer store row-major grids. Widen before
// multiplication: a 51201-square terrain exceeds a signed 32-bit element count.
constexpr std::ptrdiff_t terrainGridOffset(int x, int y, int stride)
{
    return std::ptrdiff_t(x) + std::ptrdiff_t(y) * stride;
}
#endif
