/**
 * @file wolfradiostations.cpp
 * @brief WolfViewer: radio station lists, the user's music presets and the land music helpers.
 */

#include "llviewerprecompiledheaders.h"

#include "wolfradiostations.h"

#include <boost/json.hpp>

#include "llagent.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "llfloaterland.h"
#include "llhttpconstants.h"
#include "llparcel.h"
#include "llsdjson.h"
#include "llvieweraudio.h"
#include "llviewercontrol.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "roles_constants.h"
#include "wolfgrid.h"

namespace
{
    // Source: wolfstorm/php/radio_stations.php (deployed on wolfstorm.app, like weather.php).
    constexpr const char* STATIONS_URL = "https://wolfstorm.app/php/radio_stations.php";
}

std::string WolfRadioStations::sPreviewURL;

void WolfRadioStations::load()
{
    if (mLoaded || mLoading) return;
    mLoading = true;
    mLastError.clear();
    LLCoros::instance().launch("WolfRadioStations::load", []() { WolfRadioStations::loadCoro(); });
}

// static
void WolfRadioStations::loadCoro()
{
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfRadioStations", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
    options->setTimeout(60);     // the first request of a day builds the list on the server
    LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");

    LLSD result = adapter->getRawAndSuspend(request, STATIONS_URL, options, headers);
    LLSD httpResults = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(httpResults);

    LLSD reply;
    if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
    {
        const LLSD::Binary& bytes = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(std::string(bytes.begin(), bytes.end()), ec);
        if (!ec) reply = LlsdFromJson(v);
    }

    WolfRadioStations& self = WolfRadioStations::instance();
    self.mLoading = false;
    if (reply.isMap() && reply["success"].asBoolean() && reply["genres"].isArray() && reply["featured"].isArray())
    {
        self.mFeatured = reply["featured"];
        self.mGenres = reply["genres"];
        self.mLoaded = true;
        LL_INFOS("WolfRadio") << "radio stations: " << self.mFeatured.size() << " featured, "
                              << self.mGenres.size() << " genres (" << reply["updated"].asString() << ")" << LL_ENDL;
    }
    else
    {
        self.mLastError = reply.isMap() && reply.has("error") ? reply["error"].asString()
                                                              : std::string("The station list could not be loaded (") + status.toString() + ").";
        LL_WARNS("WolfRadio") << "radio stations: " << self.mLastError << LL_ENDL;
    }
    self.mChanged();
}

// ---- presets -----------------------------------------------------------------------------

// static
std::string WolfRadioStations::preset(S32 index)
{
    LLSD p = gSavedSettings.getLLSD("WolfMusicPresets");
    return (p.isArray() && index >= 0 && index < (S32)p.size()) ? p[index].asString() : std::string();
}

// static
void WolfRadioStations::setPreset(S32 index, const std::string& url)
{
    if (index < 0 || index >= PRESET_COUNT) return;
    LLSD p = gSavedSettings.getLLSD("WolfMusicPresets");
    if (!p.isArray()) p = LLSD::emptyArray();
    while ((S32)p.size() < PRESET_COUNT) p.append(std::string());
    p[index] = url;
    gSavedSettings.setLLSD("WolfMusicPresets", p);
}

// static
bool WolfRadioStations::addPreset(const std::string& url, bool* already_there)
{
    if (already_there) *already_there = false;
    S32 empty = -1;
    for (S32 i = 0; i < PRESET_COUNT; ++i)
    {
        std::string p = preset(i);
        if (p == url)
        {
            if (already_there) *already_there = true;
            return false;
        }
        if (p.empty() && empty < 0) empty = i;
    }
    if (empty < 0) return false;
    setPreset(empty, url);
    return true;
}

// ---- the land's music --------------------------------------------------------------------

// static
LLParcel* WolfRadioStations::editableParcel()
{
    LLParcelSelectionHandle sel = LLViewerParcelMgr::getInstance()->getFloatingParcelSelection();
    LLParcel* parcel = sel ? sel->getParcel() : nullptr;
    if (!parcel || !LLViewerParcelMgr::isParcelModifiableByAgent(parcel, GP_LAND_CHANGE_MEDIA)) return nullptr;
    return parcel;
}

// static
bool WolfRadioStations::setLandMusic(const std::string& url_in)
{
    LLParcel* parcel = editableParcel();
    if (!parcel) return false;
    std::string url = url_in;
    LLStringUtil::trim(url);
    // Same as the Sound tab (llpanellandaudio.cpp onCommitAny): a bare host gets http://.
    if (!url.empty() && url.find("://") == std::string::npos) url.insert(0, "http://");
    parcel->setMusicURL(url);
    LLViewerParcelMgr::getInstance()->sendParcelPropertiesUpdate(parcel);
    LLFloaterLand::refreshAll();
    return true;
}

// static
void WolfRadioStations::preview(const std::string& url)
{
    sPreviewURL = url;
    if (!url.empty())
    {
        LLViewerAudio::getInstance()->startInternetStreamWithAutoFade(url);
        return;
    }
    // Give back what the agent's own parcel plays, under the user's music settings
    // (LLViewerParcelMgr::optionallyStartMusic, the path a parcel change takes).
    LLParcel* agent_parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
    LLViewerRegion* region = gAgent.getRegion();
    if (agent_parcel && region)
    {
        LLViewerParcelMgr::optionallyStartMusic(agent_parcel->getMusicURL(), agent_parcel->getLocalID(), region->getRegionID(), true);
    }
    else
    {
        LLViewerAudio::getInstance()->startInternetStreamWithAutoFade(LLStringUtil::null);
    }
}
