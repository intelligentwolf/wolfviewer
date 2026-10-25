/**
 * @file wolffloaterradio.h
 * @brief WolfViewer: the Radio Stations floater (About Land > Sound > Radio Stations..., and
 *        Wolf World > Music & Radio > Radio Stations...).
 *
 * [2026-10-05] Paul: "another button that says Radio Stations you click on and up comes a floater
 * that looks like a radio with a genre choice this you click on to bring up a set of stations".
 * Genre buttons (the 10 Wolf Territories stations, then each genre), the stations, a dial showing
 * the one picked, and Listen (this viewer only), Set on land, Add to my presets.
 * Lists: wolfradiostations.h.
 *
 * [2026-10-17] Paul: "can you make it look more like an old radio as well". The window is now a
 * 1930s-50s valve radio drawn in code (no textures), in the manner of Tessa's Compass
 * (wolffloatercompass.cpp): a walnut cabinet with rounded shoulders and grain, a cloth speaker
 * grille behind wooden fretwork, a green "magic eye" tuning indicator that opens while you listen,
 * a glowing dial with LW / MW / SW scales printed with the genres like station names and a red
 * needle that glides to the station, ivory push-button keys for the genres, a printed station log
 * card, and bakelite knobs (Volume = the viewer's music volume, Tuning = previous / next station).
 * The real controls (the genre keys, the station list, Listen, Set on land, Add to my presets, the
 * status line) stay ordinary XUI widgets laid over the drawing, so the keyboard, focus and
 * tooltips work as before; the drawing scales with the window, which is now resizable.
 *
 * [WOLF STATIONS 2026-10-25] Paul: residents add their own stations (wolf-grid.com/wolf-stations, gridmanager
 * wolfstations.php) and the genres grow (radio_stations.php: 14 since "Mixed / Various" and "Talk"). The genre keys
 * are made in code, one per key the feed gives (no fixed count): Wolf (featured), then Residents (the feed's "wolf"
 * list, only when the feed has that key), then every genre; they wrap onto as many rows as they need. An
 * "Add your station..." button on the station log card opens the residents' page in the web browser.
 */

#ifndef WOLF_FLOATER_RADIO_H
#define WOLF_FLOATER_RADIO_H

#include "llfloater.h"
#include "llframetimer.h"
#include "llsd.h"
#include <boost/signals2.hpp>
#include <string>
#include <vector>

class LLButton;
class LLFontGL;
class LLScrollListCtrl;
class LLTextBox;
class WolfRadioBackdrop;

class WolfFloaterRadio : public LLFloater
{
    friend class WolfRadioBackdrop;   // the view behind the controls that paints the radio
public:
    WolfFloaterRadio(const LLSD& key);
    ~WolfFloaterRadio() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void reshape(S32 width, S32 height, bool called_from_parent = true) override;

private:
    // What a genre key shows: the featured stations, the residents' stations, or one genre of the feed.
    enum class KeyKind { FEATURED, RESIDENTS, GENRE };
    struct KeyInfo
    {
        KeyKind kind = KeyKind::FEATURED;
        S32 genre = -1;         // index into WolfRadioStations::genres() for KeyKind::GENRE
    };

    void refreshGenres();
    LLButton* makeKey(S32 index);      // a new ivory genre key (the XUI genre_N buttons up to w79, as code)
    S32 keyCount() const { return (S32)mKeys.size(); }
    void showGenre(S32 index);
    const LLSD& stationsOf(S32 index) const;
    const LLSD* selectedStation() const;
    void onSelectStation();
    void onListen();
    void onSetLand();
    void onAddPreset();
    void onAddStation();
    void setStatus(const std::string& text, bool error = false);
    void updateDial();

    // ---- the drawn radio ----
    // Where everything sits, in floater-local pixels; worked out by layoutRadio() on every resize
    // and whenever the set of genre keys changes.
    struct Layout
    {
        LLRect client;          // below the title bar
        LLRect cabinet;         // the wooden body (the plinth is below it)
        F32 arch_r = 0.f;       // radius of the cabinet's rounded shoulders
        F32 inner_r = 0.f;      // radius of the grille and dial corners under the shoulders
        LLRect grille;          // cloth speaker grille behind the fretwork
        F32 eye_x = 0.f, eye_y = 0.f, eye_r = 0.f;   // the magic eye
        LLRect eye_plate;       // "ON AIR" / "OFF AIR" under the eye
        LLRect dial;            // the dial glass, inside its chrome bezel
        LLRect scale;           // the LW / MW / SW scales on the glass (the needle's travel)
        LLRect card;            // the printed station log card
        LLRect card_title;      // its printed heading (the row it sits in; the heading is centred clear of add_station)
        F32 knob_r = 0.f;
        F32 vol_x = 0.f, vol_y = 0.f;     // Volume knob centre
        F32 tune_x = 0.f, tune_y = 0.f;   // Tuning knob centre
        F32 knob_label_y = 0.f;           // vertical centre of "VOLUME" / "TUNING"
        LLRect status_plate;    // the dark maker's plate the status line sits on
    };
    void layoutRadio();
    void layoutKeys(const LLRect& area, S32 rows);
    S32 keyRows(S32 width) const;      // how many rows of genre keys a key area this wide needs

    // Painted by WolfRadioBackdrop::draw() under the widgets (floater-local coordinates).
    void drawRadio();
    void drawCabinet() const;
    void drawGrille() const;
    void drawMagicEye() const;
    void drawDial() const;
    void drawButtons() const;
    void drawCard() const;
    void drawKnob(F32 cx, F32 cy, F32 angle, const std::string& label) const;
    void drawStatusPlate() const;
    void updateAnimation();
    F32 needleTarget() const;          // 0..1 across the scale: the genre's slot, then the station in it
    S32 visibleGenres() const;         // genre keys showing (0 until the lists load)

    // The knobs, from the backdrop's mouse handlers.
    enum class Knob { NONE, VOLUME, TUNING };
    Knob knobAt(S32 x, S32 y) const;
    void turnKnob(Knob knob, S32 steps);   // +1 = clockwise: louder / next station
    std::string knobToolTip(Knob knob) const;
    LLRect knobRect(Knob knob) const;
    bool onAir() const;

    std::vector<LLButton*> mGenre;     // the keys made so far (children of the floater); the first keyCount() show
    std::vector<KeyInfo> mKeys;        // what each showing key is, from the last load
    LLScrollListCtrl* mStations = nullptr;
    LLTextBox* mStatus = nullptr;
    LLButton* mListen = nullptr;
    LLButton* mSetLand = nullptr;
    LLButton* mAddPreset = nullptr;
    LLButton* mAddStation = nullptr;
    WolfRadioBackdrop* mBackdrop = nullptr;
    S32 mGenreIndex = 0;
    boost::signals2::scoped_connection mChanged;

    // What the dial glass shows (was the dial_station / dial_detail text boxes).
    std::string mDialName;
    std::string mDialDetail;

    // Lettering from the XUI (floater_wolf_radio.xml) so it can be translated.
    std::string mOnAirLabel;
    std::string mOffAirLabel;
    std::string mLogTitle;
    std::string mVolumeLabel;
    std::string mTuningLabel;
    std::vector<std::string> mBands;          // "LW", "MW", "SW"
    std::vector<std::string> mScaleNumbers;   // the kHz figures along the top of the dial
    std::string mScaleUnit;

    Layout mLayout;
    F32 mNeedle = 0.5f;        // shown needle position, 0..1, eased towards needleTarget()
    F32 mEye = 0.f;            // 0 off .. 1 on air, eased so the eye opens and the dial warms gently
    bool mPrimed = false;      // the next frame snaps instead of easing (first frame after opening)
    LLFrameTimer mFrameTimer;
};

#endif // WOLF_FLOATER_RADIO_H
