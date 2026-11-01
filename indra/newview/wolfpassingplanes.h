/**
 * @file wolfpassingplanes.h
 * @brief [PASSING PLANES 2026-11-01] Region/Estate > Region: "Passing planes over this region".
 *
 * Paul 2026-11-01: "owners should default to passing planes off and turn it on in world region/estate", then "airport ones
 * can have it defaulted on". The switch is the record's `ambient` (region-terrain-work/wolfnpc-release/PROTOCOL.md s31),
 * read and saved through wolfstorm.app php/wolf_ambient.php "view" / "set" as the resident (X-Wolf-Agent + X-Wolf-Session,
 * as the Automatic Environment tab). Only the estate owner or an estate manager may change it (the grid checks; the same
 * rule as wolf-air's own "/74 traffic on|off"). After a save the viewer says "/74 traffic on|off" on the region so the
 * running wolf-air plugin takes it at once.
 *
 * Copyright (C) 2026 IntelligentWolf Ltd.
 */

#ifndef WOLF_PASSING_PLANES_H
#define WOLF_PASSING_PLANES_H

class LLPanel;
class LLViewerRegion;

namespace WolfPassingPlanes
{
    /** LLPanelRegionGeneralInfo::postBuild: the check box's own commit (never initCtrl: it is not part of Apply). */
    void postBuild(LLPanel* panel);
    /** After every setCtrlsEnabled of the Region tab (refreshFromRegion, processRegionInfo): shows the controls on Wolf
     *  Territories only and sets them from what is known. Reads the region's choice when it is new to the tab, when
     *  `reread` (refreshFromRegion: the window opened or the region changed), or 10 s after a failed read. */
    void sync(LLPanel* panel, LLViewerRegion* region, bool reread);
}

#endif
