#include "llparceloverlaygrid.h"
#include <cassert>
#include <cstdint>
#include <iostream>
int main() {
    for (int grid : {64,128,256,1024,4096,6400,12800,25600}) {
        int edge=parcelOverlayTextureEdge(grid,4096);
        assert(edge>0 && edge<=4096 && edge<=grid);
        assert(std::int64_t(edge)*edge*4<=67108864);
        const int chunks=std::int64_t(grid)*grid/PARCEL_OVERLAY_PAYLOAD_BYTES;
        assert(parcelOverlayChunkFits(0,grid));
        assert(parcelOverlayChunkFits(chunks-1,grid));
        assert(!parcelOverlayChunkFits(-1,grid));
        assert(!parcelOverlayChunkFits(chunks,grid));
        assert(!parcelOverlayChunkFits(INT32_MAX,grid));
        int previous=-1;
        for (int pixel=0;pixel<edge;++pixel) {
            int cell=parcelOverlaySourceCell(pixel,grid,edge);
            assert(cell>=0 && cell<grid && cell>previous);
            if(grid==edge) assert(cell==pixel);
            previous=cell;
        }
    }
    std::cout << "PASS: ordinary-region identity; bounded 25600/51200/102400-m overlays; source cell bounds and monotonic mapping\n";
}
