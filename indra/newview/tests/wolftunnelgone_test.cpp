// [T934 2026-11-19] Off-line checks of wolftunnelgone.h (the pattern of wolftunnelcamera_test.cpp: plain asserts, no viewer
// libraries). Build and run:
//   g++ -std=c++17 -O1 -Wall -Wextra -o /tmp/wtg indra/newview/tests/wolftunnelgone_test.cpp && /tmp/wtg
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include "../wolftunnelgone.h"

using namespace WolfTunnelGone;

int main()
{
    // Source: llselectmgr.h:83-98 EDeRezDestination
    assert(leavesWorld(4));    // DRD_TAKE_INTO_AGENT_INVENTORY (Take)
    assert(leavesWorld(6));    // DRD_TRASH (Delete)
    assert(leavesWorld(9));    // DRD_RETURN_TO_OWNER (Return)
    assert(leavesWorld(10));   // DRD_RETURN_TO_LAST_OWNER
    assert(!leavesWorld(0));   // DRD_SAVE_INTO_AGENT_INVENTORY (save back: stays)
    assert(!leavesWorld(1));   // DRD_ACQUIRE_TO_AGENT_INVENTORY (Take Copy: stays)
    assert(!leavesWorld(2) && !leavesWorld(3) && !leavesWorld(5) && !leavesWorld(7) && !leavesWorld(8));

    // Ticket 934's own record: "Tunnel 1", rock 6fa45100-..., 31 boxes. Her deleted rock is picked, whatever its case.
    const std::vector<std::vector<std::string>> recs = {
        { "6fa45100-5b30-4150-93c6-6fa32aa98e9c" },   // Tunnel 1 (built)
        {},                                           // a drawn tunnel never built: no objects
        { "11111111-2222-3333-4444-555555555555" },   // another built tunnel
    };
    std::vector<size_t> hit = recordsHit(recs, { "6FA45100-5B30-4150-93C6-6FA32AA98E9C" });
    assert(hit.size() == 1 && hit[0] == 0);

    // Deleting something else (her "Skye Birch Grove Tunnel" build, a plain object) touches no tunnel.
    assert(recordsHit(recs, { "5291bfda-d513-4840-9c96-a48da12e7adc" }).empty());
    // Nothing deleted, or no records: nothing.
    assert(recordsHit(recs, {}).empty());
    assert(recordsHit({}, { "6fa45100-5b30-4150-93c6-6fa32aa98e9c" }).empty());
    // Two rocks deleted at once: both tunnels, each once, in record order.
    hit = recordsHit(recs, { "11111111-2222-3333-4444-555555555555", "6fa45100-5b30-4150-93c6-6fa32aa98e9c",
                             "6fa45100-5b30-4150-93c6-6fa32aa98e9c" });
    assert(hit.size() == 2 && hit[0] == 0 && hit[1] == 2);
    // A different length or one changed character is a different id.
    assert(!sameId("6fa45100", "6fa45100-"));
    assert(!sameId("6fa45100-5b30-4150-93c6-6fa32aa98e9c", "6fa45100-5b30-4150-93c6-6fa32aa98e9d"));

    std::printf("wolftunnelgone: all checks passed\n");
    return 0;
}
