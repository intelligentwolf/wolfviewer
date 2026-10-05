/**
 * @file wolffloaterradio.cpp
 * @brief WolfViewer: the Radio Stations floater. See wolffloaterradio.h.
 */

#include "llviewerprecompiledheaders.h"

#include "wolffloaterradio.h"

#include "llbutton.h"
#include "llfloaterland.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "lltextbox.h"
#include "llviewercontrol.h"
#include "wolfradiostations.h"

namespace
{
    const LLColor4 STATUS_OK(0.95f, 0.88f, 0.72f, 1.f);      // cream on the walnut cabinet
    const LLColor4 STATUS_ERROR(1.f, 0.55f, 0.45f, 1.f);     // light red, readable on the same
}

WolfFloaterRadio::WolfFloaterRadio(const LLSD& key) : LLFloater(key)
{
}

WolfFloaterRadio::~WolfFloaterRadio()
{
}

bool WolfFloaterRadio::postBuild()
{
    for (S32 i = 0; i < GENRE_BUTTONS; ++i)
    {
        mGenre[i] = getChild<LLButton>(llformat("genre_%d", i));
        mGenre[i]->setCommitCallback([this, i](LLUICtrl*, const LLSD&) { showGenre(i); });
    }
    mStations = getChild<LLScrollListCtrl>("stations");
    mStations->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSelectStation(); });
    mStations->setDoubleClickCallback([this]() { if (!mListen->getToggleState()) { mListen->setToggleState(true); } onListen(); });
    mDialStation = getChild<LLTextBox>("dial_station");
    mDialDetail = getChild<LLTextBox>("dial_detail");
    mStatus = getChild<LLTextBox>("status");
    mListen = getChild<LLButton>("listen_btn");
    mListen->setCommitCallback([this](LLUICtrl*, const LLSD&) { onListen(); });
    mSetLand = getChild<LLButton>("set_land_btn");
    mSetLand->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSetLand(); });
    mAddPreset = getChild<LLButton>("add_preset_btn");
    mAddPreset->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAddPreset(); });

    mChanged = WolfRadioStations::instance().onChanged([this]()
    {
        refreshGenres();
        if (!WolfRadioStations::instance().isLoaded()) setStatus(WolfRadioStations::instance().lastError(), true);
    });
    return true;
}

void WolfFloaterRadio::onOpen(const LLSD& key)
{
    WolfRadioStations& st = WolfRadioStations::instance();
    st.load();
    refreshGenres();
    if (st.isLoading()) setStatus("Tuning in: loading the stations...");
}

void WolfFloaterRadio::onClose(bool app_quitting)
{
    // A preview is for while the radio is open: give the land's music back.
    if (!app_quitting && !WolfRadioStations::previewing().empty()) WolfRadioStations::preview(LLStringUtil::null);
}

const LLSD& WolfFloaterRadio::stationsOf(S32 index) const
{
    static const LLSD none = LLSD::emptyArray();
    WolfRadioStations& st = WolfRadioStations::instance();
    if (index == 0) return st.featured();
    const LLSD& genres = st.genres();
    if (index - 1 < (S32)genres.size()) return genres[index - 1]["stations"];
    return none;
}

void WolfFloaterRadio::refreshGenres()
{
    WolfRadioStations& st = WolfRadioStations::instance();
    const LLSD& genres = st.genres();
    for (S32 i = 0; i < GENRE_BUTTONS; ++i)
    {
        const bool have = st.isLoaded() && (i == 0 || i - 1 < (S32)genres.size());
        mGenre[i]->setVisible(have);
        if (have) mGenre[i]->setLabel(i == 0 ? std::string("Wolf") : genres[i - 1]["name"].asString());
    }
    mGenre[0]->setToolTip(LLStringExplicit("The Wolf Territories stations, WTGR first"));
    if (!st.isLoaded()) return;

    // The genre picked last time (setting WolfRadioGenre, a radio_stations.php genre id).
    const std::string last = gSavedSettings.getString("WolfRadioGenre");
    S32 index = 0;
    for (S32 i = 0; i < (S32)genres.size() && i + 1 < GENRE_BUTTONS; ++i)
    {
        if (genres[i]["id"].asString() == last) index = i + 1;
    }
    showGenre(index);
}

void WolfFloaterRadio::showGenre(S32 index)
{
    mGenreIndex = index;
    for (S32 i = 0; i < GENRE_BUTTONS; ++i) mGenre[i]->setToggleState(i == index);
    const LLSD& genres = WolfRadioStations::instance().genres();
    gSavedSettings.setString("WolfRadioGenre", index == 0 ? std::string("featured") : genres[index - 1]["id"].asString());

    mStations->deleteAllItems();
    const LLSD& list = stationsOf(index);
    for (S32 i = 0; i < (S32)list.size(); ++i)
    {
        const LLSD& s = list[i];
        LLSD row;
        row["value"] = i;
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = s["name"].asString();
        row["columns"][1]["column"] = "country";
        row["columns"][1]["value"] = s["country"].asString();
        row["columns"][2]["column"] = "kbps";
        row["columns"][2]["value"] = s["bitrate"].asInteger() > 0 ? llformat("%d", s["bitrate"].asInteger()) : std::string();
        mStations->addElement(row);
    }
    if (list.size() > 0) mStations->selectFirstItem();
    onSelectStation();
}

const LLSD* WolfFloaterRadio::selectedStation() const
{
    LLScrollListItem* item = mStations->getFirstSelected();
    if (!item) return nullptr;
    const LLSD& list = stationsOf(mGenreIndex);
    const S32 i = item->getValue().asInteger();
    return (i >= 0 && i < (S32)list.size()) ? &list[i] : nullptr;
}

void WolfFloaterRadio::updateDial()
{
    const LLSD* s = selectedStation();
    if (!s)
    {
        mDialStation->setText(std::string("WOLF RADIO"));
        mDialDetail->setText(std::string("Pick a genre, then a station."));
        return;
    }
    const bool on_air = !WolfRadioStations::previewing().empty() && WolfRadioStations::previewing() == (*s)["url"].asString();
    mDialStation->setText((on_air ? std::string("\xE2\x99\xAA ") : std::string()) + (*s)["name"].asString());
    std::string genre = mGenreIndex == 0 ? (*s)["genre"].asString() : mGenre[mGenreIndex]->getLabelUnselected();
    std::string detail = genre.empty() ? std::string("Wolf Territories") : genre;
    if (!(*s)["country"].asString().empty()) detail += "  \xC2\xB7  " + (*s)["country"].asString();
    if ((*s)["bitrate"].asInteger() > 0) detail += llformat("  \xC2\xB7  %d kbps", (*s)["bitrate"].asInteger());
    mDialDetail->setText(detail);
}

void WolfFloaterRadio::onSelectStation()
{
    const LLSD* s = selectedStation();
    mListen->setEnabled(s != nullptr);
    mSetLand->setEnabled(s != nullptr && WolfRadioStations::editableParcel() != nullptr);
    mAddPreset->setEnabled(s != nullptr);
    // Listening follows the selection: pick another station while listening and it plays.
    if (s && mListen->getToggleState()) WolfRadioStations::preview((*s)["url"].asString());
    if (s && !WolfRadioStations::editableParcel()) setStatus("Only someone who may change this land's media can set its music.");
    updateDial();
}

void WolfFloaterRadio::onListen()
{
    const LLSD* s = selectedStation();
    if (mListen->getToggleState() && s)
    {
        WolfRadioStations::preview((*s)["url"].asString());
        setStatus("Listening (only you hear this). Press Stop to get the land's music back.");
    }
    else
    {
        mListen->setToggleState(false);
        WolfRadioStations::preview(LLStringUtil::null);
        setStatus("Stopped. The land's own music is back.");
    }
    updateDial();
}

void WolfFloaterRadio::onSetLand()
{
    const LLSD* s = selectedStation();
    if (!s) return;
    if (!WolfRadioStations::setLandMusic((*s)["url"].asString()))
    {
        setStatus("You can't change the music on this land.", true);
        return;
    }
    // The land now plays it for everyone; stop the private preview so it is not heard twice.
    if (mListen->getToggleState())
    {
        mListen->setToggleState(false);
        WolfRadioStations::preview(LLStringUtil::null);
    }
    setStatus("Now playing on this land: " + (*s)["name"].asString());
    updateDial();
}

void WolfFloaterRadio::onAddPreset()
{
    const LLSD* s = selectedStation();
    if (!s) return;
    bool already = false;
    if (WolfRadioStations::addPreset((*s)["url"].asString(), &already))
    {
        LLFloaterLand::refreshAll();
        setStatus("Added to My presets on the Sound tab: " + (*s)["name"].asString());
    }
    else if (already)
    {
        setStatus("That station is already in My presets.");
    }
    else
    {
        setStatus("My presets are full. Clear one on the Sound tab first.", true);
    }
}

void WolfFloaterRadio::setStatus(const std::string& text, bool error)
{
    mStatus->setText(text);
    mStatus->setColor(error ? STATUS_ERROR : STATUS_OK);
}
