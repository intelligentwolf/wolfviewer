/**
 * @file wolfmiddaysky.cpp
 * @brief WolfViewer: the Midday sky with fill light (see wolfmiddaysky.h).
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

// [WolfViewer 2026-10-30] Indi: the PBR Midday is too dark - shaded sides and avatars nearly black. Paul: "fix it please
// whichever way is best". The grid's Midday (KNOWN_SKY_MIDDAY, "MiddayRP2") has ambient 0 and reflection_probe_ambiance 1,
// so a surface's light in the shade is the reflection probes' irradiance alone (class3/deferred/reflectionProbeF.glsl
// tapIrradianceMap: irradiance x ambiance, mixed over the sky's ambient by min(ambiance, 1) - at 1 the ambient is thrown
// away), and in a sunny scene the dynamic exposure sits at its floor of 0.5 (pipeline.cpp: 1 / (2 sqrt(gamma)); exposureF.glsl).
// This sky is that asset with reflection_probe_ambiance 2: the probes' irradiance counts twice, so the shade is lit by the
// sky and the sunlit ground it sees, as before, only brighter - nothing else in the sky, sun, haze, water or shaders
// changes, and every other sky looks as its author made it. The values and the arithmetic:
// firestorm repo region-terrain-work/midday-fill-2026-10-30/README.md. "Midday (Legacy)" is untouched.

#include "llviewerprecompiledheaders.h"
#include "wolfmiddaysky.h"

#include <memory>

#include "lldir.h"
#include "llenvironment.h"
#include "llfile.h"
#include "llsdserialize.h"
#include "llsettingsvo.h"
#include "llviewercontrol.h"

namespace
{
    // The last sky apply() put on: what isMidday recognises. A weak pointer, so a sky the environment has let go of is not
    // kept alive here; the Personal Lighting floater or any other change puts a different sky on, which is not "Midday".
    std::weak_ptr<LLSettingsSky> sApplied;

    /** The sky file, read once per session (a bundled file does not change while the viewer runs, so a failure is not
     *  retried either). Undefined if it is missing, unreadable or not a valid sky. */
    const LLSD& skyData()
    {
        static bool s_read = false;
        static LLSD s_data;
        if (s_read)
        {
            return s_data;
        }
        s_read = true;

        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "wolf_midday_sky.xml");
        llifstream file(path.c_str(), std::ios::in | std::ios::binary);
        LLSD data;
        // Source: llxml/llcontrol.cpp:1107 - LLSDSerialize::fromXML, PARSE_FAILURE on a bad file
        if (!file.is_open() || LLSDParser::PARSE_FAILURE == LLSDSerialize::fromXML(data, file) || !data.isMap())
        {
            LL_WARNS("WolfMidday") << "Could not read " << path << " - Midday uses the grid's Midday sky" << LL_ENDL;
            return s_data;
        }
        // LLSettingsVOSky::buildSky (llsettingsvo.cpp:465-477) logs a failed validation but builds the sky anyway, so it is
        // checked here: a damaged file falls back to the grid's sky instead of putting a broken one on.
        LLSettingsSky::validation_list_t validations = LLSettingsSky::validationList();
        LLSD check = data;
        if (!LLSettingsBase::settingValidation(check, validations)["success"].asBoolean())
        {
            LL_WARNS("WolfMidday") << path << " is not a valid sky - Midday uses the grid's Midday sky" << LL_ENDL;
            return s_data;
        }
        s_data = data;
        return s_data;
    }
}

namespace WolfMiddaySky
{
    LLSettingsSky::ptr_t apply()
    {
        LLEnvironment& env = LLEnvironment::instance();
        const LLSD& data = skyData();
        if (data.isUndefined())
        {
            // Source: llviewermenu.cpp LLWorldEnvSettings::handleEvent "noon" (stock FIRE-29926) - the grid's Midday asset
            env.setManualEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::KNOWN_SKY_MIDDAY);
            env.setSelectedEnvironment(LLEnvironment::ENV_LOCAL);
            return LLSettingsSky::ptr_t();
        }

        // A new sky each time, from the file: a sky put on earlier may have been changed in place since.
        LLSettingsSky::ptr_t sky = LLSettingsVOSky::buildSky(data);
        sApplied = sky;
        // Source: llenvironment.cpp:1586-1590 setManualEnvironment (the transition time) and :1610-1629 onSetEnvAssetLoaded
        // (setEnvironment with the loaded sky alone - the water stays the current one, :1497-1505); :1345-1356
        // setSelectedEnvironment selects ENV_LOCAL and runs updateEnvironment with that transition.
        const LLSettingsBase::Seconds transition(static_cast<F64>(gSavedSettings.getF32("FSEnvironmentManualTransitionTime")));
        env.setEnvironment(LLEnvironment::ENV_LOCAL, sky);
        env.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, transition);
        return sky;
    }

    bool isMidday(const LLSettingsSky::ptr_t& sky)
    {
        if (!sky)
        {
            return false;
        }
        // the grid's Midday asset (the fallback above, or put on before this version) or the sky apply() put on
        return sky->getAssetId() == LLEnvironment::KNOWN_SKY_MIDDAY || sky == sApplied.lock();
    }
}
