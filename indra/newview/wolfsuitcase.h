/**
 * @file wolfsuitcase.h
 * @brief WolfViewer: inventory on a hypergrid trip - landmarks into My Suitcase while away, and what arrived in
 *        the suitcase on the trip moved into the normal folders back home.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef WOLF_SUITCASE_H
#define WOLF_SUITCASE_H

#include "lluuid.h"

// [SUITCASE 2026-10-10] Paul, hypergridded to OSFest: "i tried to save a landmark on a foreign region but it wont do
// it because it needs to go in the briefcase", "surely if we pick something up from a foreign region we can transfer
// it into its inventory proper?". On another grid a visitor's home inventory takes writes only inside My Suitcase
// (OpenSim HGSuitcaseInventoryService.cs:345-356 AddItem "is not within Suitcase tree"); Firestorm (and so this
// viewer) always asked for the home Landmarks folder (lllandmarkactions.cpp createLandmarkHere,
// llfloatercreatelandmark.cpp mLandmarksID). And everything that arrives abroad lands in the suitcase.
namespace WolfSuitcase
{
    /** Standing on a region of another grid than the one this viewer logged in to (a hypergrid trip). */
    bool onForeignGrid();
    /** Where a new landmark goes: the Landmarks folder at home; My Suitcase's Landmarks abroad (the suitcase
     *  itself if it has none; null when there is no suitcase - the region then picks, Scene.Inventory.cs:140). */
    LLUUID landmarkFolder();
    /** Watch the suitcase while away; move what arrived on the trip home once back. From LLAppViewer idle. */
    void idle();
}

#endif // WOLF_SUITCASE_H
