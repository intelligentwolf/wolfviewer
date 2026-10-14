/**
 * @file wolfvehiclerez.h
 * @brief WolfViewer: the HUDs' REZ button - rez the free Wolf vehicle for that HUD and sit in it.
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

#ifndef WOLF_VEHICLEREZ_H
#define WOLF_VEHICLEREZ_H

#include <string>

#include "wolfdrive.h"

// [HUD REZ 2026-10-12] Paul: "can we have a rez button on each hud that rezzes the vehicle". His decisions:
//  - REZ always rezzes the FREE Wolf vehicle for that HUD's type: an inventory object whose description starts
//    "wolfvehicle:<type>" (car, truck, bike, speedboat, spaceship, submarine, plane, helicopter, sailboat), found in
//    the agent's own inventory first (a folder named "Free Wolf Vehicles" preferred), then in the Library;
//  - once it is in-world the avatar is sat in it;
//  - nothing is ever taken back or deleted (Paul 10-12: "dont delete the vehicle then let the user do it").
// The HUDs that call this exist only on Wolf Territories (WolfDrive / WolfFlight gate them); rez() checks again.
namespace WolfVehicleRez
{
    /** The description word for a dashboard design: car, truck, bike, speedboat, spaceship, submarine. */
    std::string typeForDesign(WolfDrive::EType design);
    /** The description word for the flight deck now: sailboat (Sailing Mode), helicopter or plane. */
    std::string typeForFlightDeck();

    /** REZ pressed on a HUD of `type`. Every failure is said in a visible alert. */
    void rez(const std::string& type);
    /** A rez is under way (finding the item, copying it from the Library, waiting for it to appear, sitting):
     *  the HUDs light their REZ button. */
    bool pending();
    /** The button's tooltip for a HUD of `type`. */
    std::string tip(const std::string& type);
}

#endif // WOLF_VEHICLEREZ_H
