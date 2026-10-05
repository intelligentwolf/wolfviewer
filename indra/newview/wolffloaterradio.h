/**
 * @file wolffloaterradio.h
 * @brief WolfViewer: the Radio Stations floater (About Land > Sound > Radio Stations...).
 *
 * [2026-10-05] Paul: "another button that says Radio Stations you click on and up comes a floater
 * that looks like a radio with a genre choice this you click on to bring up a set of stations".
 * Genre buttons (the 10 Wolf Territories stations, then each genre), the stations, a dial showing
 * the one picked, and Listen (this viewer only), Set on land, Add to my presets.
 * Lists: wolfradiostations.h.
 */

#ifndef WOLF_FLOATER_RADIO_H
#define WOLF_FLOATER_RADIO_H

#include "llfloater.h"
#include "llsd.h"
#include <boost/signals2.hpp>

class LLButton;
class LLScrollListCtrl;
class LLTextBox;

class WolfFloaterRadio : public LLFloater
{
public:
    WolfFloaterRadio(const LLSD& key);
    ~WolfFloaterRadio() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;

private:
    static constexpr S32 GENRE_BUTTONS = 13;     // the Wolf Territories stations + 12 genres

    void refreshGenres();
    void showGenre(S32 index);
    const LLSD& stationsOf(S32 index) const;
    const LLSD* selectedStation() const;
    void onSelectStation();
    void onListen();
    void onSetLand();
    void onAddPreset();
    void setStatus(const std::string& text, bool error = false);
    void updateDial();

    LLButton* mGenre[GENRE_BUTTONS] = {};
    LLScrollListCtrl* mStations = nullptr;
    LLTextBox* mDialStation = nullptr;
    LLTextBox* mDialDetail = nullptr;
    LLTextBox* mStatus = nullptr;
    LLButton* mListen = nullptr;
    LLButton* mSetLand = nullptr;
    LLButton* mAddPreset = nullptr;
    S32 mGenreIndex = 0;
    boost::signals2::scoped_connection mChanged;
};

#endif // WOLF_FLOATER_RADIO_H
