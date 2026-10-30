/**
 * @file wolfmiddaysky.h
 * @brief WolfViewer: the Midday sky that World > Environment > Midday, the EEP toolbar button and Builders Mode
 *        put on the local environment - the grid's EEP Midday with fill light in the shade.
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

#ifndef WOLF_MIDDAY_SKY_H
#define WOLF_MIDDAY_SKY_H

#include "llsettingssky.h"

namespace WolfMiddaySky
{
    /** Puts the Midday sky on ENV_LOCAL and selects it, with the resident's manual transition time
     *  (FSEnvironmentManualTransitionTime), as LLEnvironment::setManualEnvironment does for an asset. The sky is
     *  app_settings/wolf_midday_sky.xml; if that cannot be read, the grid's Midday asset (KNOWN_SKY_MIDDAY) as before.
     *  The caller checks RLV @setenv, as the menu does. Returns the sky put on, or null when the asset was asked for. */
    LLSettingsSky::ptr_t apply();

    /** True if `sky` is a Midday this viewer put on with apply() (either form): the menu's tick, its "same item
     *  again goes back to the shared environment" test and Builders Mode's "is the sky still ours" test. */
    bool isMidday(const LLSettingsSky::ptr_t& sky);
}

#endif // WOLF_MIDDAY_SKY_H
