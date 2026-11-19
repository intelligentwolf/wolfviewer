/**
 * @file wolftunnelgone.h
 * [T934 2026-11-19] Which tunnels go when the user deletes, takes or returns objects (pure, no viewer libraries, so
 * tests/wolftunnelgone_test.cpp can check it on its own).
 *
 * Ticket 934 (Indi, Indigo Blue Realm): she deleted her tunnel's rock with the ordinary Delete, and the three mouths stayed as
 * holes in her hills. The holes are the region's walkable boxes (WolfTunnelsModule.cs, the "wolf_tunnels" region setting): the
 * terrain is not drawn inside them (wolffloatertunnels.cpp bindTerrain), and only the Tunnels window's own Delete took them
 * away (deleteCoro -> takeAway). Now the viewer does the same when the tunnel's rock leaves the world any other way.
 *
 * Copyright (C) 2026 IntelligentWolf Ltd.
 */
#pragma once

#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

namespace WolfTunnelGone
{
    /**
     * Does a DeRezObject with this destination take the object out of the world?
     * Source: llselectmgr.h:83-98 EDeRezDestination - DRD_TAKE_INTO_AGENT_INVENTORY = 4 ("delete from world"), DRD_TRASH = 6,
     * DRD_RETURN_TO_OWNER = 9, DRD_RETURN_TO_LAST_OWNER = 10. Every other one leaves it in place (1 take copy, 5 god copy,
     * 0 save back) or is about attachments / task inventory (2, 3, 7, 8).
     */
    inline bool leavesWorld(int destination)
    {
        return destination == 4 || destination == 6 || destination == 9 || destination == 10;
    }

    /** Ids compared as php/tunnels.php stores them (lower case, trimmed: tun_validate_built) and LLUUID::asString writes them. */
    inline bool sameId(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
        return true;
    }

    /**
     * The index of every record whose built objects (a tunnel's rock, or a railway tunnel's bore: php/tunnels.php
     * built.objects) include one of the objects that left. A record that was never built has no objects and is never picked.
     */
    inline std::vector<size_t> recordsHit(const std::vector<std::vector<std::string>>& built_objects, const std::vector<std::string>& gone)
    {
        std::vector<size_t> out;
        for (size_t r = 0; r < built_objects.size(); ++r)
        {
            bool hit = false;
            for (const std::string& o : built_objects[r])
            {
                for (const std::string& g : gone) if (sameId(o, g)) { hit = true; break; }
                if (hit) break;
            }
            if (hit) out.push_back(r);
        }
        return out;
    }
}
