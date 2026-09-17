#ifndef LL_LLPARCELOVERLAYGRID_H
#define LL_LLPARCELOVERLAYGRID_H

#include <algorithm>
#include <cstdint>

// Source: LLViewerParcelOverlay allocates an RGBA image from the parcel grid;
// llimage.h provides the caller's supported image dimension. Keep small grids exact.
inline int parcelOverlayTextureEdge(int grid_edge, int maximum_image_edge)
{
    return std::min(grid_edge, maximum_image_edge);
}

// Source: LLDrawPoolTerrain::renderOwnership uses normalized region texture coordinates.
// Sampling each texel centre keeps the colour image aligned with that full region,
// while LLViewerParcelOverlay::ownership retains the original full-resolution grid.
inline int parcelOverlaySourceCell(int pixel, int grid_edge, int texture_edge)
{
    return static_cast<int>((2 * std::int64_t(pixel) + 1) * grid_edge / (2 * std::int64_t(texture_edge)));
}

// Source: LLViewerParcelMgr::processParcelOverlay accepts exactly 1024 payload bytes.
constexpr int PARCEL_OVERLAY_PAYLOAD_BYTES = 1024;
inline bool parcelOverlayChunkFits(int chunk, int grid_edge)
{
    return chunk >= 0 && (std::int64_t(chunk) + 1) * PARCEL_OVERLAY_PAYLOAD_BYTES
        <= std::int64_t(grid_edge) * grid_edge;
}

#endif
