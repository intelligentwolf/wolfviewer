/**
 * @file wolfestateaccess.h
 * @brief [TICKET 544 2026-11-05] Region/Estate > Estate: "Wolf Territories: who can come in".
 *
 * Jimmy Olsen's ticket 544, approved by Paul except "payment info on file" (we hold no payment data): an estate owner or
 * manager ticks no NPCs, no animesh, a minimum avatar height, WolfViewer and WolfStorm only, and no hypergrid visitors,
 * for every region of the estate (Paul 2026-11-05: no 18+ box; age gating is the Adult Content region type). The rules are read and saved through wolfstorm.app
 * php/wolf_estate_access.php "view" / "set" as the resident (X-Wolf-Agent + X-Wolf-Session, as the Region tab's Wolf
 * switches, wolfpassingplanes.cpp) and enforced on the regions by the wolf-estate-access plugin
 * (region-terrain-work/estate-access-t544/); this window only edits them. Wolf Territories only; elsewhere the block is
 * gone and the stock Estate tab is untouched.
 *
 * Copyright (C) 2026 IntelligentWolf Ltd.
 */

#ifndef WOLF_ESTATE_ACCESS_H
#define WOLF_ESTATE_ACCESS_H

class LLPanel;
class LLViewerRegion;

namespace WolfEstateAccess
{
    /** LLPanelEstateInfo::postBuild: the controls' own commits (never initCtrl: they are not part of Apply), and the stock
     *  controls' starting places, so the block can be put in and taken out again. */
    void postBuild(LLPanel* panel);
    /** After every setCtrlsEnabled of the Estate tab (LLPanelEstateInfo::updateControls) and from refreshFromRegion: shows the
     *  block on Wolf Territories only and sets it from what is known. Reads the estate's rules when the region is new to the
     *  tab, when `reread` (the window opened or the region changed), or 10 s after a failed read. */
    void sync(LLPanel* panel, LLViewerRegion* region, bool reread);
}

#endif
