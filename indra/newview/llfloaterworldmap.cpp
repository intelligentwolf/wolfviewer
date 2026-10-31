/**
 * @file llfloaterworldmap.cpp
 * @author James Cook, Tom Yedwab
 * @brief LLFloaterWorldMap class implementation
 *
 * $LicenseInfo:firstyear=2003&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

/*
 * Map of the entire world, with multiple background images,
 * avatar tracking, teleportation by double-click, etc.
 */

#include "llviewerprecompiledheaders.h"

#include "llfloaterworldmap.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llbutton.h"
#include "llcallingcard.h"
#include "llcombobox.h"
#include "llcheckboxctrl.h"
#include "llsliderctrl.h"
#include "llviewercontrol.h"
#include "llcommandhandler.h"
#include "lldraghandle.h"
//#include "llfirstuse.h"
#include "llfloaterreg.h"       // getTypedInstance()
#include "llfocusmgr.h"
//#include "lliconctrl.h" // <FS:Ansariel> Use own expand/collapse function
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llinventoryobserver.h"
#include "lllandmarklist.h"
#include "llsearcheditor.h"
#include "llnotificationsutil.h"
#include "llregionhandle.h"
#include "llscrolllistctrl.h"
#include "llviewernetwork.h" // <FS/> Access to GridManager
#include "lfsimfeaturehandler.h" // <FS/> hyperGridURL()
#include "llslurl.h"
#include "lltextbox.h"
#include "lltoolbarview.h"
#include "lltracker.h"
#include "lltrans.h"
#include "llurlregistry.h" // <FS:TJ/> Access to isUrl
#include "llviewerinventory.h"  // LLViewerInventoryItem
#include "llviewermenu.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llviewerstats.h"
#include "llviewertexture.h"
#include "llviewerwindow.h"
#include "llworldmap.h"
#include "llworldmapmessage.h"
#include "llworldmapview.h"
#include "lluictrlfactory.h"
#include "llappviewer.h"
#include "llmapimagetype.h"
#include "llweb.h"
#include "llsliderctrl.h"
#include "llspinctrl.h" // <FS/> setMinValue
#include "message.h"
#include "llwindow.h"           // copyTextToClipboard()
#include <algorithm>

// [RLVa:KB] - Checked: 2010-08-22 (RLVa-1.2.1a)
#include "rlvhandler.h"
// [/RLVa:KB]
#include "llsdutil.h"
#include "llsdutil_math.h"
#include "alfloaterregiontracker.h"
#include "llstartup.h"
#include "wolfmapglobe.h"   // <WolfViewer 2026-10-02/>
#include "wolfgrid.h"       // <WolfViewer 2026-10-05/> Buy Land
#include "llcorehttputil.h"
#include "llcoros.h"
#include <boost/json.hpp>
#include "llframetimer.h"      // [REGION CATEGORIES 2026-10-30/]
#include "llhttpconstants.h"   // [REGION CATEGORIES 2026-10-30/]
#include "llsdjson.h"          // [REGION CATEGORIES 2026-10-30/]
#include "wolfadultcontent.h"  // [REGION CATEGORIES 2026-10-30/] the same maturity icon as the map (LLSimInfo::getAccessIcon)
#include "lliconctrl.h"        // [REGION CATEGORIES VISIBILITY 2026-10-31/] the band's pin
#include "lluicolortable.h"    // [REGION CATEGORIES VISIBILITY 2026-10-31/] EmphasisColor, the band's accent
#include "wolfmapchart.h"      // [CHART ART 2026-10-31/] the "Chart illustrations" setting

//---------------------------------------------------------------------------
// Constants
//---------------------------------------------------------------------------

// Merov: we switched from using the "world size" (which varies depending where the user went) to a fixed
// width of 512 regions max visible at a time. This makes the zoom slider works in a consistent way across
// sessions and doesn't prevent the user to pan the world if it was to grow a lot beyond that limit.
// Currently (01/26/09), this value allows the whole grid to be visible in a 1024x1024 window.
static const S32 MAX_VISIBLE_REGIONS = 512;


const S32 HIDE_BEACON_PAD = 133;

// It would be more logical to have this inside the method where it is used but to compile under gcc this
// struct has to be here.
struct SortRegionNames
{
    inline bool operator ()(std::pair <U64, LLSimInfo*> const& _left, std::pair <U64, LLSimInfo*> const& _right)
    {
        return(LLStringUtil::compareInsensitive(_left.second->getName(), _right.second->getName()) < 0);
    }
};

enum EPanDirection
{
    PAN_UP,
    PAN_DOWN,
    PAN_LEFT,
    PAN_RIGHT
};

// Values in pixels per region
static const F32 ZOOM_MAX = 128.f;

//---------------------------------------------------------------------------
// Globals
//---------------------------------------------------------------------------

// handle secondlife:///app/worldmap/{NAME}/{COORDS} URLs
class LLWorldMapHandler : public LLCommandHandler
{
public:
    LLWorldMapHandler() : LLCommandHandler("worldmap", UNTRUSTED_THROTTLE)
    {
    }

    virtual bool canHandleUntrusted(
        const LLSD& params,
        const LLSD& query_map,
        LLMediaCtrl* web,
        const std::string& nav_type)
    {
        if (nav_type == NAV_TYPE_CLICKED
            || nav_type == NAV_TYPE_EXTERNAL)
        {
            // NAV_TYPE_EXTERNAL will be throttled
            return true;
        }

        return false;
    }

    bool handle(const LLSD& params,
                const LLSD& query_map,
                const std::string& grid,
                LLMediaCtrl* web)
    {
        // <FS:Ansariel> FIRE-17927: Viewer crashes when opening worldmap SLURL command from login panel
        if (LLStartUp::getStartupState() < STATE_STARTED)
        {
            return true;
        }
        // </FS:Ansariel>

        if (params.size() == 0)
        {
            // support the secondlife:///app/worldmap SLapp
            LLFloaterReg::showInstance("world_map", "center");
            return true;
        }

        // support the secondlife:///app/worldmap/{LOCATION}/{COORDS} SLapp
        const std::string region_name = LLURI::unescape(params[0].asString());
        S32 x = (params.size() > 1) ? params[1].asInteger() : 128;
        S32 y = (params.size() > 2) ? params[2].asInteger() : 128;
        S32 z = (params.size() > 3) ? params[3].asInteger() : 0;

        LLFloaterWorldMap::getInstance()->trackURL(region_name, x, y, z);
        LLFloaterReg::showInstance("world_map", "center");

        return true;
    }
};
LLWorldMapHandler gWorldMapHandler;

// handle secondlife:///app/worldmap_global/{GLOBAL_COORDS} URLs
class LLWorldMapGlobalHandler : public LLCommandHandler
{
public:
    LLWorldMapGlobalHandler() : LLCommandHandler("worldmap_global", UNTRUSTED_THROTTLE)
    {}

    virtual bool canHandleUntrusted(
        const LLSD& params,
        const LLSD& query_map,
        LLMediaCtrl* web,
        const std::string& nav_type)
    {
        if (nav_type == NAV_TYPE_CLICKED
            || nav_type == NAV_TYPE_EXTERNAL)
        {
            // NAV_TYPE_EXTERNAL will be throttled
            return true;
        }

        return false;
    }

    bool handle(const LLSD& params,
                const LLSD& query_map,
                const std::string& grid,
                LLMediaCtrl* web)
    {
        if (params.size() < 3)
        {
            LL_WARNS() << "Correct global coordinates are not provided." << LL_ENDL;
            return true;
        }

        LLVector3d parcel_global_pos = LLVector3d(params[0].asInteger(), params[1].asInteger(), params[2].asInteger());
        LLFloaterWorldMap* worldmap_instance = LLFloaterWorldMap::getInstance();
        if (!parcel_global_pos.isExactlyZero() && worldmap_instance)
        {
            worldmap_instance->trackLocation(parcel_global_pos);
            LLFloaterReg::showInstance("world_map", "center");
        }
        return true;
    }
};
LLWorldMapGlobalHandler gWorldMapGlobalHandler;

// SocialMap handler secondlife:///app/maptrackavatar/id
class LLMapTrackAvatarHandler : public LLCommandHandler
{
public:
    LLMapTrackAvatarHandler() : LLCommandHandler("maptrackavatar", UNTRUSTED_THROTTLE)
    {
    }

    virtual bool canHandleUntrusted(
        const LLSD& params,
        const LLSD& query_map,
        LLMediaCtrl* web,
        const std::string& nav_type)
    {
        if (params.size() < 1)
        {
            return true; // don't block, will fail later
        }

        if (nav_type == NAV_TYPE_CLICKED
            || nav_type == NAV_TYPE_EXTERNAL)
        {
            // NAV_TYPE_EXTERNAL will be throttled
            return true;
        }

        return false;
    }

    bool handle(const LLSD& params,
                const LLSD& query_map,
                const std::string& grid,
                LLMediaCtrl* web)
    {
        //Make sure we have some parameters
        if (params.size() == 0)
        {
            return false;
        }

        //Get the ID
        LLUUID id;
        if (!id.set( params[0], false ))
        {
            return false;
        }

        LLFloaterWorldMap::getInstance()->avatarTrackFromSlapp( id  );
        LLFloaterReg::showInstance( "world_map", "center" );

        return true;
    }
};
LLMapTrackAvatarHandler gMapTrackAvatar;

LLFloaterWorldMap* gFloaterWorldMap = NULL;

class LLMapInventoryObserver : public LLInventoryObserver
{
public:
    LLMapInventoryObserver() {}
    virtual ~LLMapInventoryObserver() {}
    virtual void changed(U32 mask);
};

void LLMapInventoryObserver::changed(U32 mask)
{
    // if there's a change we're interested in.
    if((mask & (LLInventoryObserver::CALLING_CARD | LLInventoryObserver::ADD |
                LLInventoryObserver::REMOVE)) != 0)
    {
        gFloaterWorldMap->inventoryChanged();
    }
}

class LLMapFriendObserver : public LLFriendObserver
{
public:
    LLMapFriendObserver() {}
    virtual ~LLMapFriendObserver() {}
    virtual void changed(U32 mask);
};

void LLMapFriendObserver::changed(U32 mask)
{
    // if there's a change we're interested in.
    if((mask & (LLFriendObserver::ADD | LLFriendObserver::REMOVE | LLFriendObserver::ONLINE | LLFriendObserver::POWERS)) != 0)
    {
        gFloaterWorldMap->friendsChanged();
    }
}

LLWorldMapParcelInfoObserver::LLWorldMapParcelInfoObserver(const LLVector3d& pos_global)
    : LLRemoteParcelInfoObserver(),
    mPosGlobal(pos_global),
    mParcelID(LLUUID::null)
{ }

LLWorldMapParcelInfoObserver::~LLWorldMapParcelInfoObserver()
{
    if (mParcelID.notNull())
    {
        LLRemoteParcelInfoProcessor::getInstance()->removeObserver(mParcelID, this);
    }
}

void LLWorldMapParcelInfoObserver::processParcelInfo(const LLParcelData& parcel_data)
{
    if (parcel_data.parcel_id == mParcelID)
    {
        LLRemoteParcelInfoProcessor::getInstance()->removeObserver(mParcelID, this);

        if (gFloaterWorldMap)
        {
            gFloaterWorldMap->processParcelInfo(parcel_data, mPosGlobal);
        }
    }
}

// virtual
void LLWorldMapParcelInfoObserver::setParcelID(const LLUUID& parcel_id)
{
    mParcelID = parcel_id;
    auto instance = LLRemoteParcelInfoProcessor::getInstance();
    instance->addObserver(mParcelID, this);
    instance->sendParcelInfoRequest(mParcelID);
}

// virtual
void LLWorldMapParcelInfoObserver::setErrorStatus(S32 status, const std::string& reason)
{
    LL_WARNS("LLWorldMapParcelInfoObserver") << "Can't handle remote parcel request." << " Http Status: " << status << ". Reason : " << reason << LL_ENDL;
}

//---------------------------------------------------------------------------
// Statics
//---------------------------------------------------------------------------

// Used as a pretend asset and inventory id to mean "landmark at my home location."
const LLUUID LLFloaterWorldMap::sHomeID( "10000000-0000-0000-0000-000000000001" );

//---------------------------------------------------------------------------
// Construction and destruction
//---------------------------------------------------------------------------


LLFloaterWorldMap::LLFloaterWorldMap(const LLSD& key)
:   LLFloater(key),
    mInventory(nullptr),
    mInventoryObserver(nullptr),
    mFriendObserver(nullptr),
    mCompletingRegionName(),
    mCompletingRegionPos(),
    mWaitingForTracker(false),
    mIsClosing(false),
    mSetToUserPosition(true),
    mProcessingSearchUpdate(false),
    mTrackedLocation(0.0,0.0,0.0),
    mTrackedStatus(LLTracker::TRACKING_NOTHING),
    mParcelInfoObserver(nullptr),
    mShowParcelInfo(false)
{
    gFloaterWorldMap = this;

    mFactoryMap["objects_mapview"] = LLCallbackMap(createWorldMapView, nullptr);

    mCommitCallbackRegistrar.add("WMap.Coordinates",    boost::bind(&LLFloaterWorldMap::onCoordinatesCommit, this));
    mCommitCallbackRegistrar.add("WMap.Location",       boost::bind(&LLFloaterWorldMap::onLocationCommit, this));
    mCommitCallbackRegistrar.add("WMap.AvatarCombo",    boost::bind(&LLFloaterWorldMap::onAvatarComboCommit, this));
    mCommitCallbackRegistrar.add("WMap.Landmark",       boost::bind(&LLFloaterWorldMap::onLandmarkComboCommit, this));
    mCommitCallbackRegistrar.add("WMap.SearchResult", [this](LLUICtrl* ctrl, const LLSD& data) { LLFloaterWorldMap::onCommitSearchResult(false); });
    mCommitCallbackRegistrar.add("WMap.GoHome",         boost::bind(&LLFloaterWorldMap::onGoHome, this));
    mCommitCallbackRegistrar.add("WMap.Teleport",       boost::bind(&LLFloaterWorldMap::onClickTeleportBtn, this));
    mCommitCallbackRegistrar.add("WMap.ShowTarget",     boost::bind(&LLFloaterWorldMap::onShowTargetBtn, this));
    mCommitCallbackRegistrar.add("WMap.ShowAgent",      boost::bind(&LLFloaterWorldMap::onShowAgentBtn, this));
    mCommitCallbackRegistrar.add("WMap.Clear",          boost::bind(&LLFloaterWorldMap::onClearBtn, this));
    mCommitCallbackRegistrar.add("WMap.CopySLURL",      boost::bind(&LLFloaterWorldMap::onCopySLURL, this));
    // <FS:Ansariel> Alchemy region tracker
    mCommitCallbackRegistrar.add("WMap.TrackRegion",    boost::bind(&LLFloaterWorldMap::onTrackRegion, this));

    gSavedSettings.getControl("PreferredMaturity")->getSignal()->connect(boost::bind(&LLFloaterWorldMap::onChangeMaturity, this));
}

// static
void* LLFloaterWorldMap::createWorldMapView(void* data)
{
    return new LLWorldMapView();
}

bool LLFloaterWorldMap::postBuild()
{
    mMapView = dynamic_cast<LLWorldMapView*>(getChild<LLPanel>("objects_mapview"));
    mMapView->setPan(0, 0, true);

    mTeleportButton = getChild<LLButton>("Teleport");
    mShowDestinationButton = getChild<LLButton>("Show Destination");
    mCopySlurlButton = getChild<LLButton>("copy_slurl");
    mGoHomeButton = getChild<LLButton>("Go Home");

    mPeopleCheck = getChild<LLCheckBoxCtrl>("people_chk");
    mInfohubCheck = getChild<LLCheckBoxCtrl>("infohub_chk");
    mLandSaleCheck = getChild<LLCheckBoxCtrl>("land_for_sale_chk");
    mEventsCheck = getChild<LLCheckBoxCtrl>("event_chk");
    mEventsMatureCheck = getChild<LLCheckBoxCtrl>("events_mature_chk");
    mEventsAdultCheck = getChild<LLCheckBoxCtrl>("events_adult_chk");

    mAvatarIcon = getChild<LLUICtrl>("friends_icon"); // <FS:Ansariel> Used to be avatar_icon
    mLandmarkIcon = getChild<LLUICtrl>("landmark_icon");
    mLocationIcon = getChild<LLUICtrl>("location_icon");

    // <FS> [FIRE-35333] OpenSim needs to be able to adjust the minValue
    /*
    mTeleportCoordSpinX = getChild<LLUICtrl>("teleport_coordinate_x");
    mTeleportCoordSpinY = getChild<LLUICtrl>("teleport_coordinate_y");
    mTeleportCoordSpinZ = getChild<LLUICtrl>("teleport_coordinate_z");
    */
    mTeleportCoordSpinX = getChild<LLSpinCtrl>("teleport_coordinate_x");
    mTeleportCoordSpinY = getChild<LLSpinCtrl>("teleport_coordinate_y");
    mTeleportCoordSpinZ = getChild<LLSpinCtrl>("teleport_coordinate_z");
    // </FS>

    mFriendCombo = getChild<LLComboBox>("friend combo");
    mFriendCombo->selectFirstItem();
    mFriendCombo->setPrearrangeCallback(boost::bind(&LLFloaterWorldMap::onAvatarComboPrearrange, this));
    mFriendCombo->setTextChangedCallback(boost::bind(&LLFloaterWorldMap::onComboTextEntry, this));

    mLocationEditor = getChild<LLSearchEditor>("location");
    mLocationEditor->setFocusChangedCallback(boost::bind(&LLFloaterWorldMap::onLocationFocusChanged, this, _1));
    mLocationEditor->setTextChangedCallback(boost::bind(&LLFloaterWorldMap::onSearchTextEntry, this));

    mSearchResults = getChild<LLScrollListCtrl>("search_results");
    mSearchResults->setDoubleClickCallback(boost::bind(&LLFloaterWorldMap::onClickTeleportBtn, this));

    mLandmarkCombo = getChild<LLComboBox>("landmark combo");
    mLandmarkCombo->selectFirstItem();
    mLandmarkCombo->setPrearrangeCallback(boost::bind(&LLFloaterWorldMap::onLandmarkComboPrearrange, this));
    mLandmarkCombo->setTextChangedCallback(boost::bind(&LLFloaterWorldMap::onComboTextEntry, this));

    mZoomSlider = getChild<LLSliderCtrl>("zoom slider");
    F32 slider_zoom = mMapView->getZoom();
    mZoomSlider->setValue(slider_zoom);

    mTrackCtrlsPanel = getChild<LLPanel>("layout_panel_4");
    mSearchButton = getChild<LLButton>("DoSearch");

    // [REGION CATEGORIES 2026-10-30] "Regions by Category". search_results sits under it in the XUI; when it is hidden
    // (off Wolf Territories) search_results moves up by the gap between their tops.
    // findChild: a skin without it simply has no category search (nothing moves, nothing is fetched).
    mWolfCatCombo = findChild<LLComboBox>("wolf_region_category");
    if (mWolfCatCombo)
    {
        // [REGION CATEGORIES VISIBILITY 2026-10-31] The combo sits in an accent band (floater_world_map.xml
        // wolf_region_category_band); the band is what shows/hides and what search_results moves into. Its rect is in
        // the same parent (layout_panel_4) as search_results, as the combo's was before.
        mWolfCatBand = findChild<LLPanel>("wolf_region_category_band");
        if (mWolfCatBand)
        {
            mWolfCatHeading = mWolfCatBand->findChild<LLTextBox>("wolf_region_category_heading");
            mWolfCatIcon = mWolfCatBand->findChild<LLIconCtrl>("wolf_region_category_icon");
        }
        LLView* cat_top = mWolfCatBand ? static_cast<LLView*>(mWolfCatBand) : static_cast<LLView*>(mWolfCatCombo);
        mWolfCatCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { wolfCatOnCommit(); });
        mWolfCatResultsShift = cat_top->getRect().mTop - mSearchResults->getRect().mTop;
        mWolfCatShown = true;
        wolfCatShowPrompt();
        wolfCatUpdateVisibility();
    }
    else
    {
        mWolfCatShown = false;
    }
    // [MAP EXPERIENCES 2026-10-31] Paul: "an Experiences button at the top of the map that opens the Experiences
    // floater". The Wolf World > Wolf Experiences... floater (menu_viewer.xml WolfExperiences, llviewerfloaterreg.cpp
    // "wolf_experiences"). Shown on Wolf Territories only, by wolfCatUpdateVisibility, as the category band.
    mWolfExperiencesBtn = findChild<LLButton>("wolf_map_experiences");
    if (mWolfExperiencesBtn)
    {
        // [WOLF ONLY 2026-10-31] inert off Wolf whatever its visibility says: the click re-checks the gate itself
        mWolfExperiencesBtn->setCommitCallback([this](LLUICtrl*, const LLSD&)
        {
            if (!WolfGrid::isOnWolfTerritoriesConfirmed())   // the strict test for acting (see wolfCatUpdateVisibility)
            {
                wolfCatUpdateVisibility();
                return;
            }
            LLFloaterReg::showInstance("wolf_experiences");
        });
        mWolfExperiencesBtn->setVisible(WolfGrid::isOnWolfTerritories());
    }
    // [CHART ART 2026-10-31] "Chart illustrations": the account's setting (WolfMapChartArt), Wolf only like the band.
    mWolfChartArtCheck = findChild<LLCheckBoxCtrl>("wolf_map_chart_art");
    if (mWolfChartArtCheck)
    {
        mWolfChartArtCheck->setCommitCallback([this](LLUICtrl*, const LLSD&)
        {
            WolfMapChart::setSettingOn(mWolfChartArtCheck->getValue().asBoolean());
        });
        mWolfChartArtCheck->setValue(WolfMapChart::settingOn());
        mWolfChartArtCheck->setVisible(WolfGrid::isOnWolfTerritories());
    }

    // <FS:Ansariel> Use own expand/collapse function
    //getChild<LLPanel>("expand_btn_panel")->setMouseDownCallback(boost::bind(&LLFloaterWorldMap::onExpandCollapseBtn, this));

    mTrackCtrlsPanel->setDefaultBtn(nullptr);

    onChangeMaturity();

    return true;
}

// virtual
LLFloaterWorldMap::~LLFloaterWorldMap()
{
    if (mParcelInfoObserver)
    {
        delete mParcelInfoObserver;
    }

    // All cleaned up by LLView destructor
    mMapView = NULL;

    // Inventory deletes all observers on shutdown
    mInventory = NULL;
    mInventoryObserver = NULL;

    // avatar tracker will delete this for us.
    mFriendObserver = NULL;

    gFloaterWorldMap = NULL;

    mTeleportFinishConnection.disconnect();
}

//static
LLFloaterWorldMap* LLFloaterWorldMap::getInstance()
{
    return LLFloaterReg::getTypedInstance<LLFloaterWorldMap>("world_map");
}

// virtual
void LLFloaterWorldMap::onClose(bool app_quitting)
{
    // While we're not visible, discard the overlay images we're using
    LLWorldMap::getInstance()->clearImageRefs();
    mTeleportFinishConnection.disconnect();
    // [REGION CATEGORIES PINS 2026-10-31] Paul: pins "cleared when the category changes or the floater closes"; the
    // list goes too (the next open shows the prompt), and an answer still coming is dropped.
    if (mWolfCatCombo)
    {
        ++mWolfCatSeq;
        wolfCatForget();
        wolfCatClearPins();
        mWolfCatCombo->clear();
        wolfCatShowPrompt();
        if (mSearchResults)
        {
            mSearchResults->operateOnAll(LLCtrlListInterface::OP_DELETE);
            mSearchResults->setCommentText(LLStringUtil::null);
        }
        mWolfCatListShowing = false;
    }
}

// <WolfViewer 2026-10-05> Buy Land. Paul: "can we add a "Buy Land" on the world map if a user is
// from wolf territories and doesn't have a region". Shown only on Wolf Territories, and only when
// wolf-grid.com says this avatar owns no region (gridmanager/hasregion.php, the same "owns a region"
// rule as the free radio stream). Asked each time the map opens, so a region bought this session
// hides it next time.
namespace
{
    const char* WOLF_HAS_REGION_API = "https://www.wolf-grid.com/hasregion.php";

    void wolf_update_buy_land(LLFloater* map)
    {
        LLButton* btn = map->findChild<LLButton>("wolf_buy_land");
        if (!btn) return;
        if (!WolfGrid::isOnWolfTerritories())   // <WolfViewer 2026-10-06/> and not while visiting another grid
        {
            // The request carries this login's agent and session ids: only ever to Wolf's own site.
            btn->setVisible(false);
            return;
        }
        const LLHandle<LLFloater> handle = map->getHandle();
        LLCoros::instance().launch("WolfBuyLand", [handle]()
        {
            LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
                std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfBuyLand", LLCore::HttpRequest::DEFAULT_POLICY_ID);
            LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
            LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
            options->setTimeout(15);
            LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
            headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
            headers->append("X-Wolf-Agent", gAgentID.asString());
            headers->append("X-Wolf-Session", gAgentSessionID.asString());
            LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
            LLSD result = adapter->postRawAndSuspend(request, WOLF_HAS_REGION_API, raw, options, headers);

            // {success, ownsRegion, regionsUrl}. Anything else (an error, no answer): leave it hidden.
            bool show = false;
            std::string url;
            if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
            {
                const LLSD::Binary& bin = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
                boost::system::error_code ec;
                boost::json::value root = boost::json::parse(std::string(bin.begin(), bin.end()), ec);
                if (!ec && root.is_object())
                {
                    const boost::json::object& o = root.as_object();
                    const boost::json::value* ok = o.if_contains("success");
                    const boost::json::value* owns = o.if_contains("ownsRegion");
                    const boost::json::value* u = o.if_contains("regionsUrl");
                    show = ok && ok->is_bool() && ok->as_bool() && owns && owns->is_bool() && !owns->as_bool();
                    if (u && u->is_string()) url = u->as_string().c_str();
                }
            }
            LLFloater* map = handle.get();
            LLButton* btn = map ? map->findChild<LLButton>("wolf_buy_land") : nullptr;
            if (!btn) return;
            if (url.empty()) url = "https://www.wolf-grid.com/index.php?f=ns";
            btn->setCommitCallback([url](LLUICtrl*, const LLSD&) { LLWeb::loadURLExternal(url); });
            // Paul 10-05: "make the buy land button our usual green with white writing please it
            // gets a bit lost on the map" - the green of WolfStorm's Get Land (css/viewer_menu.css
            // .get-land-btn #15803d).
            static const LLUIColor green(LLColor4(0x15 / 255.f, 0x80 / 255.f, 0x3d / 255.f, 1.f));
            static const LLUIColor white(LLColor4::white);
            btn->setImageColor(green);
            btn->setUnselectedLabelColor(white);
            btn->setSelectedLabelColor(white);
            btn->setVisible(show);
        });
    }
}
// </WolfViewer>

// [REGION CATEGORIES 2026-10-30] "Regions by Category" on the map search. Paul: "allow users to add categories to
// their regions, and then add those category buttons to the MAP SEARCH so someone looking for shops can just click
// 'SHOPS' and get a list of shopping regions". Owners choose up to three categories (the destination guide's list)
// on their region page at wolf-grid.com; gridmanager regioncats.php answers (public, read-only, JSON, cached 60 s):
//   ?list=1 -> {"ok":true,"categories":[{"id","name","count"}]}
//   ?cat=N  -> {"ok":true,"category":{"id","name"},"regions":[{"name","x","y","w","h","access","owner","shop","desc"}]}
// x/y are map cells, access the maturity byte the map itself sends (MapSearchModule.cs:178 block.Access = r.Access).
// Only listed, online regions are in it. Wolf Territories only: hidden on any other grid.
namespace
{
    const char* const WOLF_REGION_CATS_API = "https://wolf-grid.com/regioncats.php";
    const F64 WOLF_CAT_LIST_SECS = 60.0;   // regioncats.php RC_CACHE_TTL
    const S32 WOLF_CAT_MAX_REGIONS = 200;  // regioncats.php RC_MAX_REGIONS
    const S32 WOLF_CAT_MAX_CATS = 100;     // grid.destcat holds 13
    const F64 WOLF_CAT_PENDING_SECS = 10.0; // how long Teleport waits for the map to learn a chosen region

    // Source: wolfairports.cpp fetchCoro (adapter, options, Accept header) and json_to_llsd. Runs in a coroutine.
    // Returns the reply when it is {"ok":true,...}, otherwise an undefined LLSD.
    LLSD wolf_cat_get(const std::string& url, const char* what)
    {
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfRegionCats", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
        options->setTimeout(15);
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");

        LLSD result = adapter->getRawAndSuspend(request, url, options, headers);
        LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(
            result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
        if (!status)
        {
            LL_WARNS("WolfRegionCats") << what << ": " << status.toString() << LL_ENDL;
            return LLSD();
        }
        LLSD reply;
        if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
        {
            const LLSD::Binary& bytes = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
            boost::system::error_code ec;
            boost::json::value v = boost::json::parse(std::string(bytes.begin(), bytes.end()), ec);
            if (!ec) reply = LlsdFromJson(v);
        }
        if (!reply.isMap() || !reply["ok"].asBoolean())
        {
            LL_WARNS("WolfRegionCats") << what << ": unexpected reply" << LL_ENDL;
            return LLSD();
        }
        return reply;
    }
}

// [WOLF ONLY 2026-10-31] Paul: the band and the Experiences button "must only work on the Wolf Territories grid".
// The ONE test for both is WolfGrid::isOnWolfTerritories() (wolfgrid.h): logged in to Wolf AND the region underfoot
// says, in its own SimulatorFeatures GridURL, that it is Wolf's - so a hypergrid jump, another OpenSim grid and Second
// Life (not a Wolf login) all say no. Called from postBuild, onOpen and draw() (every frame the map is drawn, on any
// change of the answer). That lenient test only SHOWS things (it keeps the last region's answer while a new region's
// features are on their way, so Wolf-to-Wolf crossings do not flicker). Everything that ACTS - fetching the list or a
// category, choosing one, a pin click, the Experiences button, answers arriving - uses the strict
// isOnWolfTerritoriesConfirmed() (this region has itself said it is Wolf's), so a hypergrid hop is inert at once,
// before the foreign region's features arrive and hide the controls.
void LLFloaterWorldMap::wolfCatUpdateVisibility()
{
    const bool wolf = WolfGrid::isOnWolfTerritories();
    if (mWolfExperiencesBtn) mWolfExperiencesBtn->setVisible(wolf);   // [MAP EXPERIENCES 2026-10-31/] first: a skin may lack the band
    if (mWolfChartArtCheck)                                             // [CHART ART 2026-10-31/] the same gate
    {
        mWolfChartArtCheck->setVisible(wolf);
        mWolfChartArtCheck->setValue(WolfMapChart::settingOn());        // the account's (its settings load after login)
    }
    if (!mWolfCatCombo || !mSearchResults) return;
    if (wolf != mWolfCatShown)
    {
        LLRect rect = mSearchResults->getRect();
        rect.mTop += wolf ? -mWolfCatResultsShift : mWolfCatResultsShift;
        mSearchResults->setShape(rect);
        mWolfCatShown = wolf;
    }
    mWolfCatCombo->setVisible(wolf);
    if (mWolfCatBand)
    {
        mWolfCatBand->setVisible(wolf);   // [REGION CATEGORIES VISIBILITY 2026-10-31/]
        if (wolf) wolfCatApplyBandColours();
    }
    if (!wolf)
    {
        wolfCatClearPins();   // [REGION CATEGORIES PINS 2026-10-31/]
        // [WOLF ONLY 2026-10-31] a category's regions in the list go too (a name search's results are left alone)
        if (mWolfCatListShowing)
        {
            mSearchResults->operateOnAll(LLCtrlListInterface::OP_DELETE);
            mSearchResults->setCommentText(LLStringUtil::null);   // "No X regions..." / a failure message too
            mWolfCatListShowing = false;
        }
        // Another grid: forget Wolf's list, so coming back reads it again.
        ++mWolfCatSeq;
        wolfCatForget();
        mWolfCatCombo->removeall();
        wolfCatShowPrompt();   // [REGION CATEGORIES VISIBILITY 2026-10-31] removeall() leaves the old button label
        mWolfCatNames.clear();
        mWolfCatListAt = -1.0;
        mWolfCatListTriedAt = -1.0;   // [WOLF ONLY 2026-10-31] back on Wolf: ask at once
    }
}

void LLFloaterWorldMap::wolfCatFetchList()
{
    if (!mWolfCatCombo || !mWolfCatShown || mWolfCatListFetching) return;
    // [WOLF ONLY 2026-10-31] never asked for unless THIS region has said it is Wolf's (the strict test; draw() calls
    // this every frame, so the list comes as soon as the region's features do). A failed read is retried after 10 s.
    if (!WolfGrid::isOnWolfTerritoriesConfirmed()) return;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (mWolfCatListAt >= 0.0 && now - mWolfCatListAt < WOLF_CAT_LIST_SECS) return;
    if (mWolfCatListTriedAt >= 0.0 && now - mWolfCatListTriedAt < 10.0) return;
    mWolfCatListTriedAt = now;
    mWolfCatListFetching = true;
    const LLHandle<LLFloater> handle = getHandle();
    LLCoros::instance().launch("WolfRegionCatsList", [handle]()
    {
        const LLSD reply = wolf_cat_get(std::string(WOLF_REGION_CATS_API) + "?list=1", "category list");
        LLFloaterWorldMap* self = dynamic_cast<LLFloaterWorldMap*>(handle.get());
        if (!self) return;
        self->mWolfCatListFetching = false;
        if (!self->mWolfCatShown || !self->mWolfCatCombo || !WolfGrid::isOnWolfTerritoriesConfirmed()) return;   // left Wolf Territories meanwhile
        if (!reply["categories"].isArray())
        {
            if (self->mWolfCatNames.empty() && self->mSearchResults->getItemCount() == 0)
            {
                self->wolfCatShowComment(LLTrans::getString("worldmap_category_list_failed"));
            }
            return;
        }
        self->mWolfCatListAt = LLFrameTimer::getElapsedSeconds();
        const LLSD selected = self->mWolfCatCombo->getValue();
        self->mWolfCatCombo->removeall();
        self->mWolfCatNames.clear();
        // [REGION CATEGORIES SORT 2026-10-31] Paul: "sort the category list alphabetically". The server sorts too
        // (regioncatlib.php rcNameCmp); sorted here as well so an older answer still reads A-Z. compareDict is
        // case-insensitive and numbers compare by value, like the region list's own sort (LLScrollListCtrl).
        struct Cat { S32 id; std::string name; S32 count; };
        std::vector<Cat> cats;
        for (const LLSD& c : llsd::inArray(reply["categories"]))
        {
            if (cats.size() >= (size_t)WOLF_CAT_MAX_CATS) break;
            const std::string name = c["name"].asString();
            if (!c["id"].isInteger() || c["id"].asInteger() <= 0 || name.empty() || name.size() > 100) continue;
            cats.push_back({ c["id"].asInteger(), name, c["count"].asInteger() });
        }
        std::stable_sort(cats.begin(), cats.end(), [](const Cat& a, const Cat& b)
        {
            const S32 d = LLStringUtil::compareDict(a.name, b.name);
            return d != 0 ? d < 0 : a.id < b.id;
        });
        for (const Cat& c : cats)
        {
            if (self->mWolfCatNames.count(c.id)) continue;   // a repeated id: the first one
            LLStringUtil::format_map_t args;
            args["[CATEGORY]"] = c.name;
            args["[COUNT]"] = llformat("%d", c.count);
            self->mWolfCatCombo->add(LLTrans::getString("worldmap_category_item", args), LLSD(c.id));
            self->mWolfCatNames[c.id] = c.name;
        }
        // keep the category being shown chosen; [REGION CATEGORIES VISIBILITY 2026-10-31] if it is gone (or none was
        // chosen), removeall() left the old button label, so show the prompt instead
        if (!(selected.isInteger() && selected.asInteger() > 0 && self->mWolfCatCombo->selectByValue(selected)))
        {
            self->wolfCatShowPrompt();
        }
    });
}

void LLFloaterWorldMap::wolfCatOnCommit()
{
    if (!mWolfCatCombo || !mWolfCatShown) return;
    if (!WolfGrid::isOnWolfTerritoriesConfirmed())   // [WOLF ONLY 2026-10-31] inert off Wolf, even mid-frame before draw() hides it
    {
        wolfCatUpdateVisibility();
        return;
    }
    const LLSD value = mWolfCatCombo->getValue();
    const S32 id = value.isInteger() ? value.asInteger() : 0;
    const auto it = mWolfCatNames.find(id);
    if (id <= 0 || it == mWolfCatNames.end()) return;
    const std::string name = it->second;

    const U32 seq = ++mWolfCatSeq;
    // An empty mCompletingRegionName makes updateSims leave this list alone when map replies arrive.
    mCompletingRegionName = "";
    mProcessingSearchUpdate = false;
    wolfCatForget();
    wolfCatClearPins();   // [REGION CATEGORIES PINS 2026-10-31] the old category's pins go at once
    LLStringUtil::format_map_t args;
    args["[CATEGORY]"] = name;
    wolfCatShowComment(LLTrans::getString("worldmap_category_loading", args));

    const LLHandle<LLFloater> handle = getHandle();
    const std::string url = std::string(WOLF_REGION_CATS_API) + "?cat=" + llformat("%d", id);
    LLCoros::instance().launch("WolfRegionCatsRegions", [handle, seq, url, name]()
    {
        const LLSD reply = wolf_cat_get(url, "category regions");
        LLFloaterWorldMap* self = dynamic_cast<LLFloaterWorldMap*>(handle.get());
        if (!self || seq != self->mWolfCatSeq) return;   // closed, or another category / a typed search came after
        if (!WolfGrid::isOnWolfTerritoriesConfirmed()) { self->wolfCatUpdateVisibility(); return; }   // [WOLF ONLY 2026-10-31/]
        if (!reply["regions"].isArray())
        {
            LLStringUtil::format_map_t failed;
            failed["[CATEGORY]"] = name;
            self->wolfCatShowComment(LLTrans::getString("worldmap_category_failed", failed));
            return;
        }
        self->wolfCatFill(reply, name);
    });
}

void LLFloaterWorldMap::wolfCatFill(const LLSD& reply, const std::string& cat_name)
{
    mWolfCatListShowing = true;   // [WOLF ONLY 2026-10-31/]
    mSearchResults->operateOnAll(LLCtrlListInterface::OP_DELETE);
    wolfCatForget();
    S32 count = 0;
    // [REGION CATEGORIES PINS 2026-10-31] a pin per region and the box round all of them (global metres)
    std::vector<LLWorldMapView::WolfCatPin> pins;
    F64 x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    for (const LLSD& r : llsd::inArray(reply["regions"]))
    {
        if (count >= WOLF_CAT_MAX_REGIONS) break;
        const std::string name = r["name"].asString();
        if (name.empty() || name.size() > 128 || !r["x"].isInteger() || !r["y"].isInteger()) continue;
        const S32 x = r["x"].asInteger();
        const S32 y = r["y"].asInteger();
        // Map cells are U16 on the wire (MapBlockRequest MinX/MinY).
        if (x < 0 || y < 0 || x > 65535 || y > 65535) continue;
        S32 access = r["access"].isInteger() ? r["access"].asInteger() : 0;
        if (access < 0 || access > 255) access = 0;
        const U64 region_handle = to_region_handle((U32)x * (U32)REGION_WIDTH_UNITS, (U32)y * (U32)REGION_WIDTH_UNITS);

        // The same row a name search adds (updateSims): id = the name, the maturity icon, the name.
        LLSD value;
        value["id"] = name;
        value["columns"][0]["column"] = "scroll_list_icon";
        value["columns"][0]["value"] = WolfAdultContent::accessIcon(static_cast<U8>(access),
                                                                     WolfAdultContent::isAdultContentAt(region_handle));
        value["columns"][0]["type"] = "icon";
        value["columns"][1]["column"] = "sim_name";
        value["columns"][1]["value"] = name;
        std::string tip = r["shop"].asString();
        const std::string desc = r["desc"].asString();
        if (!desc.empty()) tip += (tip.empty() ? "" : " - ") + desc;
        if (!tip.empty()) value["columns"][1]["tool_tip"] = tip;
        mSearchResults->addElement(value);

        std::string lower = name;
        LLStringUtil::toLower(lower);
        mWolfCatCells[lower] = std::make_pair(static_cast<U16>(x), static_cast<U16>(y));

        // w/h are metres (regioncats.php, robust.regions sizeX/sizeY); a missing or silly size counts as one region.
        const S32 w = r["w"].isInteger() && r["w"].asInteger() > 0 && r["w"].asInteger() <= 65536 ? r["w"].asInteger() : REGION_WIDTH_UNITS;
        const S32 h = r["h"].isInteger() && r["h"].asInteger() > 0 && r["h"].asInteger() <= 65536 ? r["h"].asInteger() : REGION_WIDTH_UNITS;
        const F64 gx = (F64)x * REGION_WIDTH_METERS, gy = (F64)y * REGION_WIDTH_METERS;
        pins.push_back({ name, LLVector3d(gx + w * 0.5, gy + h * 0.5, 0.0) });
        if (count == 0)
        {
            x0 = gx; y0 = gy; x1 = gx + w; y1 = gy + h;
        }
        else
        {
            x0 = llmin(x0, gx); y0 = llmin(y0, gy); x1 = llmax(x1, gx + w); y1 = llmax(y1, gy + h);
        }
        ++count;
    }
    // [REGION CATEGORIES PINS 2026-10-31] Paul: "could we zoom the map to all the entries with pins in it?" Pins on
    // every region, and the map fitted round them all with the flight deck's own fit (wolfShowArea: the area fills 85%
    // of the view; on the globe it flies there, so a category spread over the whole grid ends on the globe, which is
    // as far out as the zoom goes). 20% margin so the outer pins are not on the edge; at least one region across.
    mMapView->wolfSetCategoryPins(pins);
    if (count > 0)
    {
        wolfShowArea(LLVector3d((x0 + x1) * 0.5, (y0 + y1) * 0.5, 0.0), llmax(x1 - x0, y1 - y0) * 1.2);
    }
    if (count == 0)
    {
        LLStringUtil::format_map_t args;
        args["[CATEGORY]"] = cat_name;
        mSearchResults->setCommentText(LLTrans::getString("worldmap_category_none", args));
        return;
    }
    mSearchResults->sortByColumn("sim_name", true);
}

void LLFloaterWorldMap::wolfCatShowComment(const std::string& text)
{
    mWolfCatListShowing = true;   // [WOLF ONLY 2026-10-31/] Wolf's text in the list: cleared on leaving Wolf
    mSearchResults->operateOnAll(LLCtrlListInterface::OP_DELETE);
    mSearchResults->setCommentText(text);
}

void LLFloaterWorldMap::wolfCatForget()
{
    mWolfCatCells.clear();
    mWolfCatPending.clear();
    mWolfCatPendingTeleport = false;
}

// [REGION CATEGORIES VISIBILITY 2026-10-31] WCAG 2.x relative luminance of an sRGB colour
// (https://www.w3.org/TR/WCAG21/#dfn-relative-luminance; the 0.03928 knee is the one WCAG 2.x states).
static F32 wolf_cat_luminance(const LLColor4& c)
{
    auto lin = [](F32 v)
    {
        v = llclamp(v, 0.f, 1.f);
        return v <= 0.03928f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
    };
    return 0.2126f * lin(c.mV[VRED]) + 0.7152f * lin(c.mV[VGREEN]) + 0.0722f * lin(c.mV[VBLUE]);
}

// [REGION CATEGORIES VISIBILITY 2026-10-31] The band's background is the skin's EmphasisColor (floater_world_map.xml),
// which is orange, teal, blue, pink, brown, lime, white or black depending on the skin and theme - no one text colour
// is readable on all of them. Pure black or pure white, whichever contrasts more, always gives at least 4.58:1
// (the worst case is a luminance of about 0.18, where the two are equal), so the heading meets WCAG 1.4.3 on any
// accent. The pin (a white image, textures.xml Wolf_Map_Category_Icon) is tinted the same.
// Read on every open, so a colour changed in the user's colors.xml is followed.
void LLFloaterWorldMap::wolfCatApplyBandColours()
{
    if (!mWolfCatBand) return;
    const F32 lum = wolf_cat_luminance(LLUIColorTable::instance().getColor("EmphasisColor").get());
    const F32 with_black = (lum + 0.05f) / 0.05f;
    const F32 with_white = 1.05f / (lum + 0.05f);
    const LLColor4 ink = with_black >= with_white ? LLColor4::black : LLColor4::white;
    if (mWolfCatHeading) mWolfCatHeading->setColor(ink);
    if (mWolfCatIcon) mWolfCatIcon->setColor(ink);
}

// [REGION CATEGORIES VISIBILITY 2026-10-31] The prompt the combo shows while no category is chosen.
void LLFloaterWorldMap::wolfCatShowPrompt()
{
    if (mWolfCatCombo) mWolfCatCombo->setLabel(LLTrans::getString("worldmap_category_prompt"));
}

// [REGION CATEGORIES PINS 2026-10-31]
void LLFloaterWorldMap::wolfCatClearPins()
{
    if (mMapView) mMapView->wolfSetCategoryPins({});
}

// The whole region across about 70% of the view (85% of a square 1.2 times its size): the same framing as a name
// result on the globe (WolfMapGlobe::regionZoom, ~60%), on the flat map as well.
void LLFloaterWorldMap::wolfCatZoomToRegion(const LLSimInfo* info)
{
    if (!info) return;
    const F64 sx = (F64)info->getSizeX(), sy = (F64)info->getSizeY();
    LLVector3d middle = info->getGlobalOrigin();
    middle.mdV[VX] += sx * 0.5;
    middle.mdV[VY] += sy * 0.5;
    wolfShowArea(middle, llmax(sx, sy, (F64)REGION_WIDTH_METERS) * 1.2);
}

void LLFloaterWorldMap::wolfCatSelectPin(const std::string& name)
{
    if (!mSearchResults || name.empty()) return;
    if (!WolfGrid::isOnWolfTerritoriesConfirmed()) { wolfCatUpdateVisibility(); return; }   // [WOLF ONLY 2026-10-31/]
    if (mSearchResults->selectByValue(LLSD(name)))
    {
        mSearchResults->scrollToShowSelected();
        onCommitSearchResult(false);   // the row's own path: tracks it, zooms to it, Teleport goes there
    }
}

void LLFloaterWorldMap::wolfCatResolvePending()
{
    if (mWolfCatPending.empty() || !LLWorldMap::getInstance()->simInfoFromName(mWolfCatPending)) return;
    std::string selected = mSearchResults->getSelectedValue().asString();
    LLStringUtil::toLower(selected);
    const bool still_chosen = (selected == mWolfCatPending);
    const bool from_search = mWolfCatPendingFromSearch;
    const bool teleport_now = mWolfCatPendingTeleport;
    mWolfCatPending.clear();
    mWolfCatPendingTeleport = false;
    if (!still_chosen) return;
    onCommitSearchResult(from_search);   // now the map knows it: the normal path
    if (teleport_now)
    {
        onClickTeleportBtn();
    }
}
// [/REGION CATEGORIES]

// virtual
void LLFloaterWorldMap::onOpen(const LLSD& key)
{
    wolf_update_buy_land(this);   // <WolfViewer 2026-10-05/>
    mWolfBuyLandFor = WolfGrid::isOnWolfTerritories() ? 1 : 0;   // [WOLF GRID GATE 2026-10-17/] draw() redoes it on a change
    wolfCatUpdateVisibility();    // [REGION CATEGORIES 2026-10-30] and the list, read again when over a minute old
    wolfCatFetchList();
    mTeleportFinishConnection = LLViewerParcelMgr::getInstance()->
        setTeleportFinishedCallback(boost::bind(&LLFloaterWorldMap::onTeleportFinished, this));

    bool center_on_target = (key.asString() == "center");

    mIsClosing = false;

    mMapView->clearLastClick();

    {
        // reset pan on show, so it centers on you again
        if (!center_on_target)
        {
            mMapView->setPan(0, 0, true);
        }
        mMapView->updateVisibleBlocks();

        // Reload items as they may have changed
        LLWorldMap::getInstance()->reloadItems();

        // We may already have a bounding box for the regions of the world,
        // so use that to adjust the view.
        adjustZoomSliderBounds();

        // <WolfViewer 2026-10-02> Paul: "start off like a globe and the user can spin and then
        // zoom in like google earth". Opened to show a place (center_on_target), it does not.
        static LLCachedControl<bool> start_as_globe(gSavedSettings, "WolfMapStartAsGlobe", true);
        // Applied in draw(), once the map view has its size (on a first open it may not yet).
        mWolfGlobeStartPending = start_as_globe && !center_on_target && WolfMapGlobe::instance().active();
        // </WolfViewer>

        // Could be first show
        //LLFirstUse::useMap();

        // Start speculative download of landmarks
        const LLUUID landmark_folder_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_LANDMARK);
        LLInventoryModelBackgroundFetch::instance().start(landmark_folder_id);

        if (hasFocus())
        {
            mLocationEditor->setFocus( true);
            gFocusMgr.triggerFocusFlash();
        }

        buildAvatarIDList();
        buildLandmarkIDLists();

        // If nothing is being tracked, set flag so the user position will be found
        mSetToUserPosition = ( LLTracker::getTrackingStatus() == LLTracker::TRACKING_NOTHING );
    }

    if (center_on_target)
    {
        centerOnTarget(false);
    }

    // <FS> [FIRE-35333] OpenSim allows Z coordinates to be negative based on MinSimHeight
    if (!LLGridManager::getInstance()->isInSecondLife())
    {
        LLViewerRegion* regionp = gAgent.getRegion();
        F32 min_sim_height = regionp ? regionp->getMinSimHeight() : 0.f;
        mTeleportCoordSpinZ->setMinValue(min_sim_height);
    }
    // </FS>
}

// static
void LLFloaterWorldMap::reloadIcons(void*)
{
    LLWorldMap::getInstance()->reloadItems();
}

// virtual
bool LLFloaterWorldMap::handleHover(S32 x, S32 y, MASK mask)
{
    bool handled;
    handled = LLFloater::handleHover(x, y, mask);
    return handled;
}

bool LLFloaterWorldMap::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    if (!isMinimized() && isFrontmost())
    {
        S32 map_x = x - mMapView->getRect().mLeft;
        S32 map_y = y - mMapView->getRect().mBottom;
        if (mMapView->pointInView(map_x, map_y))
        {
            mMapView->wolfStopFlight();   // <WolfViewer 2026-10-02/> a scroll takes over from a flight
            mWolfAreaFlatFollow = false;  // [REGION CATEGORIES PINS 2026-10-31/] and from a flat-map fit
            F32 old_slider_zoom = (F32) mZoomSlider->getValue().asReal();
            F32 slider_zoom     = old_slider_zoom + ((F32) clicks * -0.3333f);
            mZoomSlider->setValue(LLSD(slider_zoom));
            mMapView->zoomWithPivot(slider_zoom, map_x, map_y);
            return true;
        }
    }

    return LLFloater::handleScrollWheel(x, y, clicks);
}


// virtual
void LLFloaterWorldMap::reshape( S32 width, S32 height, bool called_from_parent )
{
    LLFloater::reshape( width, height, called_from_parent );
}


// virtual
void LLFloaterWorldMap::draw()
{
    // <FS:Ansariel> Performance improvement
    static LLButton* track_region_btn     = getChild<LLButton>("track_region");
    // [WOLF GRID GATE 2026-10-17] Buy Land (wolf_update_buy_land) was only decided on open: a hypergrid jump with the map
    // open left Wolf's button on another grid's map.
    {
        const S32 wolf = WolfGrid::isOnWolfTerritories() ? 1 : 0;
        if (wolf != mWolfBuyLandFor)
        {
            mWolfBuyLandFor = wolf;
            wolf_update_buy_land(this);   // hides it off Wolf; on Wolf asks again whether this region may be bought
            wolfCatUpdateVisibility();    // [REGION CATEGORIES 2026-10-30] the same gate for "Regions by Category"
            wolfCatFetchList();
        }
        // [WOLF ONLY 2026-10-31] the list is fetched only once THIS region has confirmed it is Wolf's, which may be a
        // moment after the map opens (or after arriving); wolfCatFetchList returns at once when there is nothing to do.
        if (mWolfCatShown && mWolfCatNames.empty())
        {
            wolfCatFetchList();
        }
    }

    static LLUIColor map_track_color = LLUIColorTable::instance().getColor("MapTrackColor", LLColor4::white);
    static LLUIColor map_track_disabled_color = LLUIColorTable::instance().getColor("MapTrackDisabledColor", LLColor4::white);

    // On orientation island, users don't have a home location yet, so don't
    // let them teleport "home".  It dumps them in an often-crowed welcome
    // area (infohub) and they get confused. JC
    LLViewerRegion* regionp = gAgent.getRegion();
    bool agent_on_prelude = (regionp && regionp->isPrelude());
    bool enable_go_home = gAgent.isGodlike() || !agent_on_prelude;
    mGoHomeButton->setEnabled(enable_go_home);

    updateLocation();

    LLTracker::ETrackingStatus tracking_status = LLTracker::getTrackingStatus();
    if (LLTracker::TRACKING_AVATAR == tracking_status)
    {
        mAvatarIcon->setColor( map_track_color);
    }
    else
    {
        mAvatarIcon->setColor( map_track_disabled_color);
    }

    if (LLTracker::TRACKING_LANDMARK == tracking_status)
    {
        mLandmarkIcon->setColor( map_track_color);
    }
    else
    {
        mLandmarkIcon->setColor( map_track_disabled_color);
    }

    if (LLTracker::TRACKING_LOCATION == tracking_status)
    {
        mLocationIcon->setColor( map_track_color);
    }
    else
    {
        if (mCompletingRegionName != "")
        {
            F64 seconds = LLTimer::getElapsedSeconds();
            double value = fmod(seconds, 2);
            value = 0.5 + 0.5*cos(value * F_PI);
            LLColor4 loading_color(0.0, F32(value/2), F32(value), 1.0);
            mLocationIcon->setColor( loading_color);
        }
        else
        {
            mLocationIcon->setColor( map_track_disabled_color);
        }
    }

    // check for completion of tracking data
    if (mWaitingForTracker)
    {
        centerOnTarget(true);
    }

    // <FS:Ansariel> Performance improvement
//  getChildView("Teleport")->setEnabled((bool)tracking_status);
//  //  getChildView("Clear")->setEnabled((bool)tracking_status);
//  getChildView("Show Destination")->setEnabled((bool)tracking_status || LLWorldMap::getInstance()->isTracking());
//  getChildView("copy_slurl")->setEnabled((mSLURL.isValid()) );
//// [RLVa:KB] - Checked: 2010-08-22 (RLVa-1.2.1a) | Added: RLVa-1.2.1a
//  childSetEnabled("Go Home",
//      (!rlv_handler_t::isEnabled()) || !(gRlvHandler.hasBehaviour(RLV_BHVR_TPLM) && gRlvHandler.hasBehaviour(RLV_BHVR_TPLOC)));
//// [/RLVa:KB]
    mTeleportButton->setEnabled((bool)tracking_status);
    mShowDestinationButton->setEnabled((bool)tracking_status || LLWorldMap::getInstance()->isTracking());
    mCopySlurlButton->setEnabled((mSLURL.isValid()) );
    mGoHomeButton->setEnabled((!rlv_handler_t::isEnabled()) || !(gRlvHandler.hasBehaviour(RLV_BHVR_TPLM) && gRlvHandler.hasBehaviour(RLV_BHVR_TPLOC)));
    // </FS:Ansariel> Performance improvement
    // <FS:Ansariel> Alchemy region tracker
    track_region_btn->setEnabled((bool) tracking_status || LLWorldMap::getInstance()->isTracking());

    setMouseOpaque(true);
    getDragHandle()->setMouseOpaque(true);

    // <WolfViewer 2026-10-02> Wolf Territories: the globe's zoom limit depends on the view's size,
    // so it is worked out again when that changes; and the open-as-globe zoom waits for a size.
    if (WolfMapGlobe::instance().active())
    {
        WolfMapGlobe& globe = WolfMapGlobe::instance();
        const LLRect view_rect = mMapView->getRect();
        const S32 w = view_rect.getWidth(), h = view_rect.getHeight();
        globe.setViewSize(w, h);   // also asks for the map's box
        if (w > 0 && h > 0)
        {
            if (w != mWolfViewWidth || h != mWolfViewHeight || globe.extentGeneration() != mWolfExtentGen)
            {
                mWolfViewWidth = w;
                mWolfViewHeight = h;
                mWolfExtentGen = globe.extentGeneration();
                adjustZoomSliderBounds();
            }
            // The globe needs the map's box (map_extent.php); until it has come, the map is flat.
            if (mWolfGlobeStartPending && globe.hasExtent())
            {
                mWolfGlobeStartPending = false;
                wolfSetMapZoom(WolfMapGlobe::instance().globeZoom(w, h));
            }
        }
    }
    // </WolfViewer>
    // <WolfViewer 2026-10-06> wolfShowArea: the area fills 85% of the view's shorter side.
    // zoom is log2(pixels per metre) (LLWorldMapView::zoomFromScale), so the view's centre goes
    // to the area's centre by a pan of -(centre - camera) * 2^zoom pixels (globalPosToView).
    if (mWolfAreaPending)
    {
        const LLRect view_rect = mMapView->getRect();
        const S32 w = view_rect.getWidth(), h = view_rect.getHeight();
        if (w > 0 && h > 0)
        {
            mWolfAreaPending = false;
            const F64 span = llmax(mWolfAreaSpan, (F64)REGION_WIDTH_METERS);
            const F32 z = (F32)log2(0.85 * llmin(w, h) / span);
            if (WolfMapGlobe::instance().active())
            {
                mMapView->wolfFlyTo(mWolfAreaCentre, z);   // the globe's own flight there
            }
            else
            {
                // [REGION CATEGORIES PINS 2026-10-31] The slider clamps z to its range; pan for the zoom it really
                // takes, and keep panning while the zoom animates (see mWolfAreaFlatFollow). Before, the pan was
                // computed once for the unclamped z and then rescaled by the zoom animation, so it landed off-centre.
                wolfSetMapZoom(z);
                mWolfAreaFlatZoom = (F32)mZoomSlider->getValue().asReal();
                mWolfAreaFlatStart = LLFrameTimer::getElapsedSeconds();
                mWolfAreaFlatFollow = true;
            }
        }
    }
    // </WolfViewer>
    mMapView->zoom((F32)mZoomSlider->getValue().asReal());
    if (mWolfAreaFlatFollow)
    {
        // pixels per metre now (getScale: pixels per region); the view's centre to the area's centre
        const LLVector3d rel = mWolfAreaCentre - gAgentCamera.getCameraPositionGlobal();
        const F64 ppm = (F64)mMapView->getScale() / REGION_WIDTH_METERS;
        mMapView->setPan(-(S32)ll_round(rel.mdV[VX] * ppm), -(S32)ll_round(rel.mdV[VY] * ppm), true);
        if (fabsf(mMapView->getZoom() - mWolfAreaFlatZoom) < 0.005f
            || LLFrameTimer::getElapsedSeconds() - mWolfAreaFlatStart > 2.0
            || WolfMapGlobe::instance().active())
        {
            mWolfAreaFlatFollow = false;
        }
    }

    // Enable/disable checkboxes depending on the zoom level
    // If above threshold level (i.e. low res) -> Disable all checkboxes
    // If under threshold level (i.e. high res) -> Enable all checkboxes
    bool enable = mMapView->showRegionInfo();
    mPeopleCheck->setEnabled(enable);
    mInfohubCheck->setEnabled(enable);
    mLandSaleCheck->setEnabled(enable);
    mEventsCheck->setEnabled(enable);
    mEventsMatureCheck->setEnabled(enable);
    mEventsAdultCheck->setEnabled(enable);

    LLFloater::draw();
}


//-------------------------------------------------------------------------
// Internal utility functions
//-------------------------------------------------------------------------

void LLFloaterWorldMap::processParcelInfo(const LLParcelData& parcel_data, const LLVector3d& pos_global) const
{
    LLVector3d tracker_pos = LLTracker::getTrackedPositionGlobal();
    if (!mShowParcelInfo ||
        (tracker_pos.mdV[VX] != pos_global.mdV[VX] && tracker_pos.mdV[VY] != pos_global.mdV[VY]) ||
        LLTracker::getTrackedLocationType() != LLTracker::LOCATION_NOTHING ||
        LLTracker::getTrackingStatus() != LLTracker::TRACKING_LOCATION)
    {
        return;
    }

    LLSimInfo* sim_info = LLWorldMap::getInstance()->simInfoFromPosGlobal(pos_global);
    if (!sim_info)
    {
        return;
    }

    std::string sim_name = sim_info->getName();
    U32 locX, locY;
    from_region_handle(sim_info->getHandle(), &locX, &locY);
    F32 region_x = (F32)(pos_global.mdV[VX] - locX);
    F32 region_y = (F32)(pos_global.mdV[VY] - locY);
    std::string full_name = llformat("%s (%d, %d, %d)",
        sim_name.c_str(),
        ll_round(region_x),
        ll_round(region_y),
        ll_round((F32)pos_global.mdV[VZ]));

    LLTracker::trackLocation(pos_global, parcel_data.name.empty() ? getString("UnnamedParcel") : parcel_data.name, full_name);
}

void LLFloaterWorldMap::requestParcelInfo(const LLVector3d& pos_global, const LLVector3d& region_origin)
{
    if (pos_global == mRequestedGlobalPos)
    {
        return;
    }

    LLViewerRegion* region = gAgent.getRegion();
    if (!region)
    {
        return;
    }

    if (std::string url = region->getCapability("RemoteParcelRequest"); !url.empty())
    {
        mRequestedGlobalPos = pos_global;
        if (mParcelInfoObserver)
        {
            delete mParcelInfoObserver;
        }
        mParcelInfoObserver = new LLWorldMapParcelInfoObserver(pos_global);

        auto pos_region = LLVector3(pos_global - region_origin);
        LLRemoteParcelInfoProcessor::instance().requestRegionParcelInfo(url,
            region->getRegionID(), pos_region, pos_global,
            mParcelInfoObserver->getObserverHandle());
    }
    else
    {
        LL_WARNS() << "Cannot request parcel details: Cap not found" << LL_ENDL;
    }
}

void LLFloaterWorldMap::trackAvatar(const LLUUID& avatar_id, const std::string& name)
{
    mShowParcelInfo = false;

    buildAvatarIDList();
    if (mFriendCombo->setCurrentByID(avatar_id) || gAgent.isGodlike())
    {
        // *HACK: Adjust Z values automatically for liaisons & gods so
        // they swoop down when they click on the map. Requested
        // convenience.
        if (gAgent.isGodlike())
        {
            mTeleportCoordSpinZ->setValue(LLSD(200.f));
        }
        // Don't re-request info if we already have it or we won't have it in time to teleport
        if (mTrackedStatus != LLTracker::TRACKING_AVATAR || avatar_id != mTrackedAvatarID)
        {
            mTrackedStatus   = LLTracker::TRACKING_AVATAR;
            mTrackedAvatarID = avatar_id;
            LLTracker::trackAvatar(avatar_id, name);
            centerOnTarget(true);
        }
    }
    else
    {
        LLTracker::stopTracking(false);
    }
    mTrackCtrlsPanel->setDefaultBtn(mTeleportButton);
}

void LLFloaterWorldMap::trackLandmark(const LLUUID& landmark_item_id)
{
    mShowParcelInfo = false;

    buildLandmarkIDLists();
    bool found = false;
    S32  idx;
    for (idx = 0; idx < mLandmarkItemIDList.size(); idx++)
    {
        if (mLandmarkItemIDList.at(idx) == landmark_item_id)
        {
            found = true;
            break;
        }
    }

    if (found && mLandmarkCombo->setCurrentByID(landmark_item_id))
    {
        LLUUID      asset_id = mLandmarkAssetIDList.at(idx);
        std::string name     = mLandmarkCombo->getSimple();
        mTrackedStatus       = LLTracker::TRACKING_LANDMARK;
        LLTracker::trackLandmark(mLandmarkAssetIDList.at(idx), // assetID
                                 mLandmarkItemIDList.at(idx),  // itemID
                                 name);                        // name

        if (asset_id != sHomeID)
        {
            // start the download process
            gLandmarkList.getAsset(asset_id);
        }
    }
    else
    {
        LLTracker::stopTracking(false);
    }
    mTrackCtrlsPanel->setDefaultBtn(mTeleportButton);
}


void LLFloaterWorldMap::trackEvent(const LLItemInfo &event_info)
{
    mShowParcelInfo = false;
    mTrackedStatus = LLTracker::TRACKING_LOCATION;
    LLTracker::trackLocation(event_info.getGlobalPosition(), event_info.getName(), event_info.getToolTip(), LLTracker::LOCATION_EVENT);
    mTrackCtrlsPanel->setDefaultBtn(mTeleportButton);
}

void LLFloaterWorldMap::trackGenericItem(const LLItemInfo &item)
{
    mShowParcelInfo = false;
    mTrackedStatus = LLTracker::TRACKING_LOCATION;
    LLTracker::trackLocation(item.getGlobalPosition(), item.getName(), item.getToolTip(), LLTracker::LOCATION_ITEM);
    mTrackCtrlsPanel->setDefaultBtn(mTeleportButton);
}

void LLFloaterWorldMap::trackLocation(const LLVector3d& pos_global)
{
    mProcessingSearchUpdate = false;
    LLSimInfo* sim_info = LLWorldMap::getInstance()->simInfoFromPosGlobal(pos_global);
    if (!sim_info)
    {
        // We haven't found a region for that point yet, leave the tracking to the world map
        LLTracker::stopTracking(false);
        LLWorldMap::getInstance()->setTracking(pos_global);
        S32 world_x = S32(pos_global.mdV[0] / 256);
        S32 world_y = S32(pos_global.mdV[1] / 256);
        LLWorldMapMessage::getInstance()->sendMapBlockRequest(world_x, world_y, world_x, world_y, true);
        mTrackCtrlsPanel->setDefaultBtn(nullptr);

        // clicked on a non-region - turn off coord display
        enableTeleportCoordsDisplay( false );

        return;
    }
    if (sim_info->isDown())
    {
        // Down region. Show the blue circle of death!
        // i.e. let the world map that this and tell it it's invalid
        LLTracker::stopTracking(false);
        LLWorldMap::getInstance()->setTracking(pos_global);
        LLWorldMap::getInstance()->setTrackingInvalid();
        mTrackCtrlsPanel->setDefaultBtn(nullptr);

        // clicked on a down region - turn off coord display
        enableTeleportCoordsDisplay( false );

        return;
    }

    std::string sim_name = sim_info->getName();
// <FS:CR> Aurora-sim var region teleports
    //F32 region_x = (F32)fmod( pos_global.mdV[VX], (F64)REGION_WIDTH_METERS );
    //F32 region_y = (F32)fmod( pos_global.mdV[VY], (F64)REGION_WIDTH_METERS );
    U32 locX, locY;
    from_region_handle(sim_info->getHandle(), &locX, &locY);
    F32 region_x = (F32)(pos_global.mdV[VX] - locX);
    F32 region_y = (F32)(pos_global.mdV[VY] - locY);
// </FS:CR>
    std::string full_name = llformat("%s (%d, %d, %d)",
                                     sim_name.c_str(),
                                     ll_round(region_x),
                                     ll_round(region_y),
                                     ll_round((F32)pos_global.mdV[VZ]));

    std::string tooltip("");
    mTrackedStatus = LLTracker::TRACKING_LOCATION;
// [RLVa:KB] - Checked: 2012-02-08 (RLVa-1.4.5) | Added: RLVa-1.4.5
    LLTracker::trackLocation(pos_global, (!gRlvHandler.hasBehaviour(RLV_BHVR_SHOWLOC)) ? full_name : RlvStrings::getString(RlvStringKeys::Hidden::Generic).c_str(), tooltip);
// [/RLVa:KB]
//  LLTracker::trackLocation(pos_global, full_name, tooltip);
    LLWorldMap::getInstance()->cancelTracking();        // The floater is taking over the tracking

    // <FS:Ansariel> RLVa support
    //mShowParcelInfo = true;
    //requestParcelInfo(pos_global, sim_info->getGlobalOrigin());
    if (!gRlvHandler.hasBehaviour(RLV_BHVR_SHOWLOC))
    {
        mShowParcelInfo = true;
        requestParcelInfo(pos_global, sim_info->getGlobalOrigin());
    }
    else
    {
        mShowParcelInfo = false;
    }
    // </FS:Ansariel> RLVa support

    LLVector3d coord_pos = LLTracker::getTrackedPositionGlobal();
    updateTeleportCoordsDisplay( coord_pos );

    // we have a valid region - turn on coord display
    enableTeleportCoordsDisplay( true );

    mTrackCtrlsPanel->setDefaultBtn(mTeleportButton);
}

// enable/disable teleport destination coordinates
void LLFloaterWorldMap::enableTeleportCoordsDisplay( bool enabled )
{
// [RLVa:KB] - Checked: 2012-02-08 (RLVa-1.4.5) | Added: RLVa-1.4.5
    LLUICtrl* pCtrl = getChild<LLUICtrl>("events_label");
    pCtrl->setVisible(!gRlvHandler.hasBehaviour(RLV_BHVR_SHOWLOC));

    mTeleportCoordSpinX->setVisible(!gRlvHandler.hasBehaviour(RLV_BHVR_SHOWLOC));
    mTeleportCoordSpinX->setEnabled(enabled);

    mTeleportCoordSpinY->setVisible(!gRlvHandler.hasBehaviour(RLV_BHVR_SHOWLOC));
    mTeleportCoordSpinY->setEnabled(enabled);

    mTeleportCoordSpinZ->setVisible(!gRlvHandler.hasBehaviour(RLV_BHVR_SHOWLOC));
    mTeleportCoordSpinZ->setEnabled(enabled);
// [/RLVa:KB]
//    mTeleportCoordSpinX->setEnabled(enabled);
//    mTeleportCoordSpinY->setEnabled(enabled);
//    mTeleportCoordSpinZ->setEnabled(enabled);
}

// update display of teleport destination coordinates - pos is in global coordinates
void LLFloaterWorldMap::updateTeleportCoordsDisplay( const LLVector3d& pos )
{
    // convert global specified position to a local one
// <FS:CR> Aurora-Sim var region support
    //F32 region_local_x = (F32)fmod( pos.mdV[VX], (F64)REGION_WIDTH_METERS );
    //F32 region_local_y = (F32)fmod( pos.mdV[VY], (F64)REGION_WIDTH_METERS );
    LLSimInfo* sim_info = LLWorldMap::getInstance()->simInfoFromPosGlobal(pos);
    if (sim_info)
    {
        // if we're going to update their value, we should also enable them
        enableTeleportCoordsDisplay( true );

        U32 locX, locY;
        from_region_handle(sim_info->getHandle(), &locX, &locY);
        F32 region_local_x = (F32)(pos.mdV[VX] - locX);
        F32 region_local_y = (F32)(pos.mdV[VY] - locY);
        //F32 region_local_z = (F32)fmod( pos.mdV[VZ], (F64)REGION_WIDTH_METERS );
        F32 region_local_z = (F32)pos.mdV[VZ];

        // write in the values
        mTeleportCoordSpinX->setValue(region_local_x);
        mTeleportCoordSpinY->setValue(region_local_y);
        mTeleportCoordSpinZ->setValue(region_local_z);
    }
// </FS:CR>
}

void LLFloaterWorldMap::updateLocation()
{
    bool gotSimName;

    LLTracker::ETrackingStatus status = LLTracker::getTrackingStatus();

    // These values may get updated by a message, so need to check them every frame
    // The fields may be changed by the user, so only update them if the data changes
    LLVector3d pos_global = LLTracker::getTrackedPositionGlobal();
    if (pos_global.isExactlyZero())
    {
        LLVector3d agentPos = gAgent.getPositionGlobal();

        // Set to avatar's current postion if nothing is selected
        if ( status == LLTracker::TRACKING_NOTHING && mSetToUserPosition )
        {
            // Make sure we know where we are before setting the current user position
            std::string agent_sim_name;
            gotSimName = LLWorldMap::getInstance()->simNameFromPosGlobal( agentPos, agent_sim_name );
// [RLVa:KB] - Checked: 2012-02-08 (RLVa-1.4.5) | Added: RLVa-1.4.5
            if (gRlvHandler.hasBehaviour(RLV_BHVR_SHOWLOC))
            {
                mSetToUserPosition = false;

                // Fill out the location field
                mLocationEditor->setValue(RlvStrings::getString(RlvStringKeys::Hidden::Region));

                // update the coordinate display with location of avatar in region
                updateTeleportCoordsDisplay( agentPos );

                mSLURL = LLSLURL();
            }
            else if (gotSimName)
// [/RLVa:KB]
//          if ( gotSimName )
            {
                mSetToUserPosition = false;

                // Fill out the location field
                mLocationEditor->setValue(agent_sim_name);

                // update the coordinate display with location of avatar in region
                updateTeleportCoordsDisplay( agentPos );

                // Figure out where user is
                // Set the current SLURL
// <FS:CR> Aurora-sim var region teleports
                //mSLURL = LLSLURL(agent_sim_name, gAgent.getPositionGlobal());
                // <FS> [FIRE-35268] OpenSim support for when on other grids
                if (LLGridManager::getInstance()->isInSecondLife())
                    mSLURL = LLSLURL(agent_sim_name, gAgent.getPositionRegion());   // <WolfViewer> region frame
                else
                    mSLURL = LLSLURL(LFSimFeatureHandler::instance().hyperGridURL(), agent_sim_name, gAgent.getPositionRegion());   // <WolfViewer> region frame
                // </FS>
// </FS:CR>
            }
        }

        return; // invalid location
    }
    std::string sim_name;
    // <FS:Beq pp Oren> FIRE-30768: SLURL's don't work in VarRegions
    //gotSimName = LLWorldMap::getInstance()->simNameFromPosGlobal( pos_global, sim_name );
    LLSimInfo* sim_info = LLWorldMap::getInstance()->simInfoFromPosGlobal(pos_global);
    if (sim_info)
        sim_name = sim_info->getName();
    // </FS:Beq pp Oren>
    if ((status != LLTracker::TRACKING_NOTHING) &&
        (status != mTrackedStatus || pos_global != mTrackedLocation || sim_name != mTrackedSimName))
    {
        mTrackedStatus = status;
        mTrackedLocation = pos_global;
        mTrackedSimName = sim_name;

        if (status == LLTracker::TRACKING_AVATAR)
        {
            // *HACK: Adjust Z values automatically for liaisons &
            // gods so they swoop down when they click on the
            // map. Requested convenience.
            if(gAgent.isGodlike())
            {
                pos_global[2] = 200;
            }
        }

        if (!mProcessingSearchUpdate)
        {
            mLocationEditor->setValue(sim_name);
        }

        // refresh coordinate display to reflect where user clicked.
        LLVector3d coord_pos = LLTracker::getTrackedPositionGlobal();
        updateTeleportCoordsDisplay( coord_pos );

        // simNameFromPosGlobal can fail, so don't give the user an invalid SLURL
// [RLVa:KB] - Checked: 2012-02-08 (RLVa-1.4.5) | Added: RLVa-1.4.5
        if (gRlvHandler.hasBehaviour(RLV_BHVR_SHOWLOC))
        {
            mSLURL = LLSLURL();
            mLocationEditor->setValue(RlvStrings::getString(RlvStringKeys::Hidden::Region));
        }
// <FS:Beq pp Oren> FORE-30768 Slurls in Var regions are b0rked
        // else if (gotSimName)
        else if (sim_info)
// [/RLVa:KB]
//      if ( gotSimName )
        {
            // mSLURL = LLSLURL(sim_name, pos_global);
            // <FS> [FIRE-35268] OpenSim support for when on other grids
            if (LLGridManager::getInstance()->isInSecondLife())
                mSLURL = LLSLURL(sim_info->getName(), sim_info->getGlobalOrigin(), pos_global);
            else
                mSLURL = LLSLURL(LFSimFeatureHandler::instance().hyperGridURL(), sim_info->getName(), sim_info->getGlobalOrigin(), pos_global);
            // </FS>
        }
// </FS:Beq pp Oren>
        else
        {   // Empty SLURL will disable the "Copy SLURL to clipboard" button
            mSLURL = LLSLURL();
        }

// [RLVa:KB] - Checked: 2009-07-04 (RLVa-1.0.0a)
/*
        if (gRlvHandler.hasBehaviour(RLV_BHVR_SHOWLOC))
        {
            mLocationEditor->setValue(RlvStrings::getString(RlvStringKeys::Hidden::Region));
            mSLURL.clear();
        }
*/
// [/RLVa:KB]
    }
}

void LLFloaterWorldMap::trackURL(const std::string& region_name, S32 x_coord, S32 y_coord, S32 z_coord)
{
    LLSimInfo* sim_info = LLWorldMap::getInstance()->simInfoFromName(region_name);
    // <FS> [FIRE-35333] OpenSim allows Z coordinates to be negative based on MinSimHeight
    if (!LLGridManager::getInstance()->isInSecondLife())
    {
        LLViewerRegion* regionp = gAgent.getRegion();
        F32 min_sim_height = regionp ? regionp->getMinSimHeight() : 0.f;
        z_coord = llclamp(z_coord, min_sim_height, 4096);
    }
    else
    {
        z_coord = llclamp(z_coord, 0, 4096);
    }
    // </FS>
    if (sim_info)
    {
        LLVector3 local_pos;
        local_pos.mV[VX] = (F32)x_coord;
        local_pos.mV[VY] = (F32)y_coord;
        local_pos.mV[VZ] = (F32)z_coord;
        LLVector3d global_pos = sim_info->getGlobalPos(local_pos);
        trackLocation(global_pos);
        mTrackCtrlsPanel->setDefaultBtn(mTeleportButton);
    }
    else
    {
        // fill in UI based on URL
        mLocationEditor->setValue(region_name);

        // Save local coords to highlight position after region global
        // position is returned.
        mCompletingRegionPos.set(
                                                   (F32)x_coord, (F32)y_coord, (F32)z_coord);

        // pass sim name to combo box
        mCompletingRegionName = region_name;
        LLWorldMapMessage::getInstance()->sendNamedRegionRequest(region_name);
        LLStringUtil::toLower(mCompletingRegionName);
        LLWorldMap::getInstance()->setTrackingCommit();
    }
}

void LLFloaterWorldMap::observeInventory(LLInventoryModel* model)
{
    if(mInventory)
    {
        mInventory->removeObserver(mInventoryObserver);
        delete mInventoryObserver;
        mInventory = NULL;
        mInventoryObserver = NULL;
    }
    if(model)
    {
        mInventory = model;
        mInventoryObserver = new LLMapInventoryObserver;
        // Inventory deletes all observers on shutdown
        mInventory->addObserver(mInventoryObserver);
        inventoryChanged();
    }
}

void LLFloaterWorldMap::inventoryChanged()
{
    if(!LLTracker::getTrackedLandmarkItemID().isNull())
    {
        LLUUID item_id = LLTracker::getTrackedLandmarkItemID();
        buildLandmarkIDLists();
        trackLandmark(item_id);
    }
}

void LLFloaterWorldMap::observeFriends()
{
    if(!mFriendObserver)
    {
        mFriendObserver = new LLMapFriendObserver;
        LLAvatarTracker::instance().addObserver(mFriendObserver);
        friendsChanged();
    }
}

void LLFloaterWorldMap::friendsChanged()
{
    LLAvatarTracker& t         = LLAvatarTracker::instance();
    const LLUUID&    avatar_id = t.getAvatarID();
    buildAvatarIDList();
    if (avatar_id.notNull())
    {
        const LLRelationship* buddy_info = t.getBuddyInfo(avatar_id);
        if (!mFriendCombo->setCurrentByID(avatar_id) ||
            (buddy_info && !buddy_info->isRightGrantedFrom(LLRelationship::GRANT_MAP_LOCATION)) || gAgent.isGodlike())
        {
            LLTracker::stopTracking(false);
        }
    }
}

// No longer really builds a list.  Instead, just updates mAvatarCombo.
void LLFloaterWorldMap::buildAvatarIDList()
{
    // Delete all but the "None" entry
    S32 list_size = mFriendCombo->getItemCount();
    if (list_size > 1)
    {
        mFriendCombo->selectItemRange(1, -1);
        mFriendCombo->operateOnSelection(LLCtrlListInterface::OP_DELETE);
    }

    // Get all of the calling cards for avatar that are currently online
    LLCollectMappableBuddies collector;
    LLAvatarTracker::instance().applyFunctor(collector);
    LLCollectMappableBuddies::buddy_map_t::iterator it;
    LLCollectMappableBuddies::buddy_map_t::iterator end;
    it  = collector.mMappable.begin();
    end = collector.mMappable.end();
    // <FS:Ansariel> Sort friend list alphabetically
    //for(; it != end; ++it)
    //{
    //  mFriendCombo->addSimpleElement((*it).second, ADD_BOTTOM, (*it).first);
    //}

    std::multimap<std::string, LLUUID> buddymap;
    for (; it != end; ++it)
    {
        buddymap.emplace(it->second, it->first);
    }
    for (const auto& [name, id] : buddymap)
    {
        mFriendCombo->addSimpleElement(name, ADD_BOTTOM, id);
    }
    // </FS:Ansariel>

    mFriendCombo->setCurrentByID(LLAvatarTracker::instance().getAvatarID());
    mFriendCombo->selectFirstItem();
}


void LLFloaterWorldMap::buildLandmarkIDLists()
{
    // Delete all but the "None" entry
    S32 list_size = mLandmarkCombo->getItemCount();
    if (list_size > 1)
    {
        mLandmarkCombo->selectItemRange(1, -1);
        mLandmarkCombo->operateOnSelection(LLCtrlListInterface::OP_DELETE);
    }

    mLandmarkItemIDList.clear();
    mLandmarkAssetIDList.clear();

    // Get all of the current landmarks
    mLandmarkAssetIDList.push_back( LLUUID::null );
    mLandmarkItemIDList.push_back( LLUUID::null );

    mLandmarkAssetIDList.push_back( sHomeID );
    mLandmarkItemIDList.push_back( sHomeID );

    LLInventoryModel::cat_array_t cats;
    LLInventoryModel::item_array_t items;
    LLIsType is_landmark(LLAssetType::AT_LANDMARK);
    gInventory.collectDescendentsIf(gInventory.getRootFolderID(),
                                    cats,
                                    items,
                                    LLInventoryModel::EXCLUDE_TRASH,
                                    is_landmark);

    std::sort(items.begin(), items.end(), LLViewerInventoryItem::comparePointers());

    mLandmarkAssetIDList.reserve(mLandmarkAssetIDList.size() + items.size());
    mLandmarkItemIDList.reserve(mLandmarkItemIDList.size() + items.size());

    // <FS:Ansariel> Filter duplicate landmarks on world map
    uuid_set_t used_landmarks;
    bool filterLandmarks = gSavedSettings.getBOOL("WorldmapFilterDuplicateLandmarks");
    // </FS:Ansariel>
    auto count = items.size();
    for(size_t i = 0; i < count; ++i)
    {
        LLInventoryItem* item = items.at(i);

        // <FS:Ansariel> Filter duplicate landmarks on world map
        if (filterLandmarks)
        {
            if (used_landmarks.contains(item->getAssetUUID()))
            {
                continue;
            }
            used_landmarks.emplace(item->getAssetUUID());
        }
        // </FS:Ansariel>

        mLandmarkCombo->addSimpleElement(item->getName(), ADD_BOTTOM, item->getUUID());

        mLandmarkAssetIDList.push_back( item->getAssetUUID() );
        mLandmarkItemIDList.push_back( item->getUUID() );
    }

    mLandmarkCombo->selectFirstItem();
}


F32 LLFloaterWorldMap::getDistanceToDestination(const LLVector3d &destination,
                                                F32 z_attenuation) const
{
    LLVector3d delta = destination - gAgent.getPositionGlobal();
    // by attenuating the z-component we effectively
    // give more weight to the x-y plane
    delta.mdV[VZ] *= z_attenuation;
    F32 distance = (F32)delta.magVec();
    return distance;
}


void LLFloaterWorldMap::clearLocationSelection(bool clear_ui, bool dest_reached)
{
    if (!dest_reached || (mSearchResults->getItemCount() == 1))
    {
        mSearchResults->operateOnAll(LLCtrlListInterface::OP_DELETE);
    }
    LLWorldMap::getInstance()->cancelTracking();
    mCompletingRegionName = "";
}


void LLFloaterWorldMap::clearLandmarkSelection(bool clear_ui)
{
    if (clear_ui || !childHasKeyboardFocus("landmark combo"))
    {
        mLandmarkCombo->selectByValue("None");
    }
}


void LLFloaterWorldMap::clearAvatarSelection(bool clear_ui)
{
    if (clear_ui || !childHasKeyboardFocus("friend combo"))
    {
        mTrackedStatus = LLTracker::TRACKING_NOTHING;
        if (mFriendCombo->getSelectedValue().asString() != "None")
        {
            mFriendCombo->selectByValue("None");
        }
    }
}


// Adjust the maximally zoomed out limit of the zoom slider so you
// can see the whole world, plus a little.
void LLFloaterWorldMap::adjustZoomSliderBounds()
{
    // Merov: we switched from using the "world size" (which varies depending where the user went) to a fixed
    // width of 512 regions max visible at a time. This makes the zoom slider works in a consistent way across
    // sessions and doesn't prevent the user to pan the world if it was to grow a lot beyond that limit.
    // Currently (01/26/09), this value allows the whole grid to be visible in a 1024x1024 window.
    S32 world_width_regions  = MAX_VISIBLE_REGIONS;
    S32 world_height_regions = MAX_VISIBLE_REGIONS;

    // Find how much space we have to display the world
    LLRect view_rect = mMapView->getRect();

    // View size in pixels
    S32 view_width = view_rect.getWidth();
    S32 view_height = view_rect.getHeight();

    // Pixels per region to display entire width/height
    F32 width_pixels_per_region = (F32) view_width / (F32) world_width_regions;
    F32 height_pixels_per_region = (F32) view_height / (F32) world_height_regions;

    F32 pixels_per_region = llmin(width_pixels_per_region,
                                  height_pixels_per_region);

    // Round pixels per region to an even number of slider increments
    S32 slider_units = llfloor(pixels_per_region / 0.2f);
    pixels_per_region = slider_units * 0.2f;

    // Make sure the zoom slider can be moved at least a little bit.
    // Likewise, less than the increment pixels per region is just silly.
    pixels_per_region = llclamp(pixels_per_region, 1.f, ZOOM_MAX);

    F32 min_power = log(pixels_per_region/256.f)/log(2.f);

    // <WolfViewer 2026-10-02/> On Wolf Territories the slider goes on out to the whole globe.
    if (WolfMapGlobe::instance().active() && WolfMapGlobe::instance().hasExtent())
    {
        min_power = llmin(min_power, WolfMapGlobe::instance().globeZoom(view_width, view_height));
    }

    mZoomSlider->setMinValue(min_power);
}

// <WolfViewer 2026-10-02>
void LLFloaterWorldMap::wolfSetMapZoom(F32 zoom)
{
    mZoomSlider->setValue(LLSD(zoom));   // the slider clamps it to its range
}

void LLFloaterWorldMap::wolfShowArea(const LLVector3d& centre, F64 span_m)
{
    mWolfAreaCentre = centre;
    mWolfAreaSpan = span_m;
    mWolfAreaPending = true;
    mWolfGlobeStartPending = false;   // shown for a place: not the opening globe
}
// </WolfViewer>


//-------------------------------------------------------------------------
// User interface widget callbacks
//-------------------------------------------------------------------------

void LLFloaterWorldMap::onGoHome()
{
    gAgent.teleportHome();
    closeFloater();
    mProcessingSearchUpdate = false;
}


void LLFloaterWorldMap::onLandmarkComboPrearrange()
{
    if (mIsClosing)
    {
        return;
    }

    LLUUID current_choice = mLandmarkCombo->getCurrentID();

    buildLandmarkIDLists();

    if (current_choice.isNull() || !mLandmarkCombo->setCurrentByID(current_choice))
    {
        LLTracker::stopTracking(false);
    }
}

void LLFloaterWorldMap::onComboTextEntry()
{
    // Reset the tracking whenever we start typing into any of the search fields,
    // so that hitting <enter> does an auto-complete versus teleporting us to the
    // previously selected landmark/friend.
    LLTracker::stopTracking(false);
}

void LLFloaterWorldMap::onSearchTextEntry( )
{
    onComboTextEntry();
    updateSearchEnabled();
}


void LLFloaterWorldMap::onLandmarkComboCommit()
{
    if (mIsClosing)
    {
        return;
    }

    LLUUID asset_id;
    LLUUID item_id = mLandmarkCombo->getCurrentID();

    LLTracker::stopTracking(false);

    // RN: stopTracking() clears current combobox selection, need to reassert it here
    mLandmarkCombo->setCurrentByID(item_id);

    if (item_id.isNull()) {}
    else if (item_id == sHomeID)
    {
        asset_id = sHomeID;
    }
    else
    {
        LLInventoryItem* item = gInventory.getItem(item_id);
        if (item)
        {
            asset_id = item->getAssetUUID();
        }
        else
        {
            // Something went wrong, so revert to a safe value.
            item_id.setNull();
        }
    }

    trackLandmark(item_id);
    onShowTargetBtn();

    // Reset to user postion if nothing is tracked
    mSetToUserPosition = (LLTracker::getTrackingStatus() == LLTracker::TRACKING_NOTHING);
}

// static
void LLFloaterWorldMap::onAvatarComboPrearrange()
{
    if (mIsClosing)
    {
        return;
    }

    LLUUID current_choice;

    if (LLAvatarTracker::instance().haveTrackingInfo())
    {
        current_choice = LLAvatarTracker::instance().getAvatarID();
    }

    buildAvatarIDList();

    if (!mFriendCombo->setCurrentByID(current_choice) || current_choice.isNull())
    {
        LLTracker::stopTracking(false);
    }
}

void LLFloaterWorldMap::onAvatarComboCommit()
{
    if (mIsClosing)
    {
        return;
    }

    const LLUUID& new_avatar_id = mFriendCombo->getCurrentID();
    if (new_avatar_id.notNull())
    {
        std::string name = mFriendCombo->getSimple();
        trackAvatar(new_avatar_id, name);
        onShowTargetBtn();
    }
    else
    { // Reset to user postion if nothing is tracked
        mSetToUserPosition = (LLTracker::getTrackingStatus() == LLTracker::TRACKING_NOTHING);
    }
}

void LLFloaterWorldMap::avatarTrackFromSlapp( const LLUUID& id )
{
    trackAvatar( id, "av" );
    onShowTargetBtn();
}

void LLFloaterWorldMap::onLocationFocusChanged( LLFocusableElement* focus )
{
    updateSearchEnabled();
}

void LLFloaterWorldMap::updateSearchEnabled()
{
    if (childHasKeyboardFocus("location") &&
        mLocationEditor->getValue().asString().length() > 0)
    {
        mTrackCtrlsPanel->setDefaultBtn(mSearchButton);
    }
    else
    {
        mTrackCtrlsPanel->setDefaultBtn(nullptr);
    }
}

void LLFloaterWorldMap::onLocationCommit()
{
    if( mIsClosing )
    {
        return;
    }

    clearLocationSelection(false);
    mCompletingRegionName = "";
    mLastRegionName = "";
    // [REGION CATEGORIES 2026-10-30] a typed search replaces a category list (and drops an answer still coming)
    ++mWolfCatSeq;
    wolfCatForget();
    if (mWolfCatCombo)
    {
        mWolfCatCombo->clear();
        wolfCatShowPrompt();   // [REGION CATEGORIES VISIBILITY 2026-10-31] clear() blanks the button; say what is inside again
        wolfCatClearPins();    // [REGION CATEGORIES PINS 2026-10-31/]
        mWolfCatListShowing = false;   // [WOLF ONLY 2026-10-31] the name search's results replace it
    }

    std::string str = mLocationEditor->getValue().asString();

    // Trim any leading and trailing spaces in the search target
    std::string saved_str = str;
    LLStringUtil::trim( str );
    if ( str != saved_str )
    {   // Set the value in the UI if any spaces were removed
        mLocationEditor->setValue(str);
    }

    // Don't try completing empty name (STORM-1427).
    if (str.empty())
    {
        return;
    }
    mProcessingSearchUpdate = true;

    LLStringUtil::toLower(str);
    mCompletingRegionName = str;
    LLWorldMap::getInstance()->setTrackingCommit();
    if (str.length() >= 3)
    {
        LLWorldMapMessage::getInstance()->sendNamedRegionRequest(str);
    }
    else
    {
        str += "#";
        LLWorldMapMessage::getInstance()->sendNamedRegionRequest(str);
    }
}

void LLFloaterWorldMap::onCoordinatesCommit()
{
    if( mIsClosing )
    {
        return;
    }
    mProcessingSearchUpdate = false;

    S32 x_coord = (S32)mTeleportCoordSpinX->getValue().asReal();
    S32 y_coord = (S32)mTeleportCoordSpinY->getValue().asReal();
    S32 z_coord = (S32)mTeleportCoordSpinZ->getValue().asReal();

    const std::string region_name = mLocationEditor->getValue().asString();

    trackURL( region_name, x_coord, y_coord, z_coord );
}

void LLFloaterWorldMap::onClearBtn()
{
    mTrackedStatus = LLTracker::TRACKING_NOTHING;
    mProcessingSearchUpdate = false;
    LLTracker::stopTracking(true);
    LLWorldMap::getInstance()->cancelTracking();
    mSLURL = LLSLURL();                 // Clear the SLURL since it's invalid
    mSetToUserPosition = true;  // Revert back to the current user position
}

void LLFloaterWorldMap::onShowTargetBtn()
{
    centerOnTarget(true);
}

void LLFloaterWorldMap::onShowAgentBtn()
{
    mMapView->setPanWithInterpTime(0, 0, false, 0.1f);  // false == animate
    // Set flag so user's location will be displayed if not tracking anything else
    mSetToUserPosition = true;
    mProcessingSearchUpdate = false;
}

void LLFloaterWorldMap::onClickTeleportBtn()
{
    // [REGION CATEGORIES 2026-10-30] A category result the map did not know yet is still being looked up
    // (wolfCatResolvePending): the tracker still points at the previous place, so teleport once it is found.
    if (!mWolfCatPending.empty())
    {
        std::string selected = mSearchResults->getSelectedValue().asString();
        LLStringUtil::toLower(selected);
        if (selected == mWolfCatPending)
        {
            if (LLFrameTimer::getElapsedSeconds() - mWolfCatPendingAt < WOLF_CAT_PENDING_SECS)
            {
                mWolfCatPendingTeleport = true;
                return;
            }
            // The map never answered for it: say so rather than teleport to whatever was tracked before.
            wolfCatForget();
            LLSD args;
            args["MESSAGE"] = LLTrans::getString("worldmap_category_not_on_map");
            LLNotificationsUtil::add("GenericAlert", args);
            return;
        }
        wolfCatForget();   // another row is chosen now: the old lookup no longer matters
    }
    teleport();
}

void LLFloaterWorldMap::onCopySLURL()
{
    getWindow()->copyTextToClipboard(utf8str_to_wstring(mSLURL.getSLURLString()));

    LLSD args;
    args["SLURL"] = mSLURL.getSLURLString();

    LLNotificationsUtil::add("CopySLURL", args);
}

// <FS:Ansariel> Use own expand/collapse function
//void LLFloaterWorldMap::onExpandCollapseBtn()
//{
//    LLLayoutStack* floater_stack = getChild<LLLayoutStack>("floater_map_stack");
//    LLLayoutPanel* controls_panel = getChild<LLLayoutPanel>("controls_lp");

//    bool toggle_collapse = !controls_panel->isCollapsed();
//    floater_stack->collapsePanel(controls_panel, toggle_collapse);
//    floater_stack->updateLayout();

//    std::string image_name = getString(toggle_collapse ? "expand_icon" : "collapse_icon");
//    std::string tooltip = getString(toggle_collapse ? "expand_tooltip" : "collapse_tooltip");
//    LLIconCtrl* expandCollapseIcon = getChild<LLIconCtrl>("expand_collapse_icon");
//    expandCollapseIcon->setImage(LLUI::getUIImage(image_name));
//    expandCollapseIcon->setToolTip(tooltip);
//    getChild<LLPanel>("expand_btn_panel")->setToolTip(tooltip);
//}
// </FS:Ansariel>

// <FS:Ansariel> Alchemy region tracker
void LLFloaterWorldMap::onTrackRegion()
{
    ALFloaterRegionTracker* floaterp = LLFloaterReg::getTypedInstance<ALFloaterRegionTracker>("region_tracker");
    if (floaterp)
    {
        if (LLTracker::getTrackingStatus() != LLTracker::TRACKING_NOTHING)
        {
            std::string sim_name;
            LLWorldMap::getInstance()->simNameFromPosGlobal(LLTracker::getTrackedPositionGlobal(), sim_name);
            if (!sim_name.empty())
            {
                const std::string& temp_label = floaterp->getRegionLabelIfExists(sim_name);
                LLSD args, payload;
                args["REGION"] = sim_name;
                args["LABEL"] = !temp_label.empty() ? temp_label : sim_name;
                payload["name"] = sim_name;
                LLNotificationsUtil::add("RegionTrackerAdd", args, payload, boost::bind(&ALFloaterRegionTracker::onRegionAddedCallback, floaterp, _1, _2));
            }
            else
            {
                LL_WARNS() << "Cannot add region to region tracker because sim name is empty." << LL_ENDL;
            }
        }
        else
        {
            LL_WARNS() << "Cannot add region to region tracker because we aren't tracking anything." << LL_ENDL;
        }
    }
}
// </FS:Ansariel>

// protected
void LLFloaterWorldMap::centerOnTarget(bool animate)
{
    LLVector3d pos_global;
    if(LLTracker::getTrackingStatus() != LLTracker::TRACKING_NOTHING)
    {
        LLVector3d tracked_position = LLTracker::getTrackedPositionGlobal();
        //RN: tracker doesn't allow us to query completion, so we check for a tracking position of
        // absolute zero, and keep trying in the draw loop
        if (tracked_position.isExactlyZero())
        {
            mWaitingForTracker = true;
            return;
        }
        else
        {
            // We've got the position finally, so we're no longer busy. JC
            //          getWindow()->decBusyCount();
            pos_global = LLTracker::getTrackedPositionGlobal() - gAgentCamera.getCameraPositionGlobal();
        }
    }
    else if(LLWorldMap::getInstance()->isTracking())
    {
        pos_global = LLWorldMap::getInstance()->getTrackedPositionGlobal() - gAgentCamera.getCameraPositionGlobal();;



    }
    else
    {
        // default behavior = center on agent
        pos_global.clearVec();
    }

    F64 map_scale = (F64)mMapView->getScale();
    mMapView->setPanWithInterpTime(-llfloor((F32)(pos_global.mdV[VX] * map_scale / REGION_WIDTH_METERS)),
                           -llfloor((F32)(pos_global.mdV[VY] * map_scale / REGION_WIDTH_METERS)),
                           !animate, 0.1f);
    mWaitingForTracker = false;
}

// protected
void LLFloaterWorldMap::fly()
{
    LLVector3d pos_global = LLTracker::getTrackedPositionGlobal();

    // Start the autopilot and close the floater,
    // so we can see where we're flying
    if (!pos_global.isExactlyZero())
    {
        gAgent.startAutoPilotGlobal( pos_global );
        closeFloater();
    }
    else
    {
        make_ui_sound("UISndInvalidOp");
    }
}


// protected
void LLFloaterWorldMap::teleport()
{
    bool teleport_home = false;
    LLVector3d pos_global;
    LLAvatarTracker& av_tracker = LLAvatarTracker::instance();

    LLTracker::ETrackingStatus tracking_status = LLTracker::getTrackingStatus();
    if (LLTracker::TRACKING_AVATAR == tracking_status
        && av_tracker.haveTrackingInfo() )
    {
        pos_global = av_tracker.getGlobalPos();
        pos_global.mdV[VZ] = mTeleportCoordSpinZ->getValue();
    }
    else if ( LLTracker::TRACKING_LANDMARK == tracking_status)
    {
        if( LLTracker::getTrackedLandmarkAssetID() == sHomeID )
        {
            teleport_home = true;
        }
        else
        {
            LLLandmark* landmark = gLandmarkList.getAsset( LLTracker::getTrackedLandmarkAssetID() );
            LLUUID region_id;
            if(landmark
               && !landmark->getGlobalPos(pos_global)
               && landmark->getRegionID(region_id))
            {
                LLLandmark::requestRegionHandle(
                                                gMessageSystem,
                                                gAgent.getRegionHost(),
                                                region_id,
                                                NULL);
            }
        }
    }
    else if ( LLTracker::TRACKING_LOCATION == tracking_status)
    {
        pos_global = LLTracker::getTrackedPositionGlobal();
    }
    else
    {
        make_ui_sound("UISndInvalidOp");
    }

    // Do the teleport, which will also close the floater
    if (teleport_home)
    {
        gAgent.teleportHome();
    }
    else if (!pos_global.isExactlyZero())
    {
        if(LLTracker::TRACKING_LANDMARK == tracking_status)
        {
            gAgent.teleportViaLandmark(LLTracker::getTrackedLandmarkAssetID());
        }
        else
        {
            gAgent.teleportViaLocation( pos_global );
        }
    }

    if (mProcessingSearchUpdate)
    {
        mProcessingSearchUpdate = false;
        mTrackedSimName.clear();
    }
}

void LLFloaterWorldMap::flyToLandmark()
{
    LLVector3d destination_pos_global;
    if( !LLTracker::getTrackedLandmarkAssetID().isNull() )
    {
        if (LLTracker::hasLandmarkPosition())
        {
            gAgent.startAutoPilotGlobal( LLTracker::getTrackedPositionGlobal() );
        }
    }
}

void LLFloaterWorldMap::teleportToLandmark()
{
    bool has_destination = false;
    LLUUID destination_id; // Null means "home"

    if( LLTracker::getTrackedLandmarkAssetID() == sHomeID )
    {
        has_destination = true;
    }
    else
    {
        LLLandmark* landmark = gLandmarkList.getAsset( LLTracker::getTrackedLandmarkAssetID() );
        LLVector3d global_pos;
        if(landmark && landmark->getGlobalPos(global_pos))
        {
            destination_id = LLTracker::getTrackedLandmarkAssetID();
            has_destination = true;
        }
        else if(landmark)
        {
            // pop up an anonymous request request.
            LLUUID region_id;
            if(landmark->getRegionID(region_id))
            {
                LLLandmark::requestRegionHandle(
                                                gMessageSystem,
                                                gAgent.getRegionHost(),
                                                region_id,
                                                NULL);
            }
        }
    }

    if( has_destination )
    {
        gAgent.teleportViaLandmark( destination_id );
    }
}


void LLFloaterWorldMap::teleportToAvatar()
{
    LLAvatarTracker& av_tracker = LLAvatarTracker::instance();
    if(av_tracker.haveTrackingInfo())
    {
        LLVector3d pos_global = av_tracker.getGlobalPos();
        gAgent.teleportViaLocation( pos_global );
    }
}


void LLFloaterWorldMap::flyToAvatar()
{
    if (LLAvatarTracker::instance().haveTrackingInfo())
    {
        gAgent.startAutoPilotGlobal(LLAvatarTracker::instance().getGlobalPos());
    }
}

void LLFloaterWorldMap::updateSims(bool found_null_sim)
{
    if (mCompletingRegionName == "")
    {
        wolfCatResolvePending();   // [REGION CATEGORIES 2026-10-30/] a category result the map has just learnt
        return;
    }

    mSearchResults->operateOnAll(LLCtrlListInterface::OP_DELETE);
    wolfCatForget();   // [REGION CATEGORIES 2026-10-30/] a name search's list replaces a category list

    // auto name_length = mCompletingRegionName.length(); // <FS:Beq/> FIRE-23591 support map search partial matches (Patch by Kevin Cozens)

    LLSD match;

    S32 num_results = 0;

    std::vector<std::pair <U64, LLSimInfo*> > sim_info_vec(LLWorldMap::getInstance()->getRegionMap().begin(), LLWorldMap::getInstance()->getRegionMap().end());
    std::sort(sim_info_vec.begin(), sim_info_vec.end(), SortRegionNames());

    for (std::vector<std::pair <U64, LLSimInfo*> >::const_iterator it = sim_info_vec.begin(); it != sim_info_vec.end(); ++it)
    {
        LLSimInfo* info = it->second;
        std::string sim_name_lower = info->getName();
        LLStringUtil::toLower(sim_name_lower);
        // <FS:Beq> FIRE-23591 support map search partial matches (Patch by Kevin Cozens)
        // if (sim_name_lower.substr(0, name_length) == mCompletingRegionName)
        if (sim_name_lower.find(mCompletingRegionName) != std::string::npos)
        // <FS:Beq>
        {
            if (sim_name_lower == mCompletingRegionName)
            {
                match = info->getName();
            }

            LLSD value;
            value["id"] = info->getName();
            // <FS:Ansariel> Show region rating if there already is an icon column
            //value["columns"][0]["column"] = "sim_name";
            //value["columns"][0]["value"] = info->getName();
            value["columns"][0]["column"] = "scroll_list_icon";
            value["columns"][0]["value"] = info->getAccessIcon();
            value["columns"][0]["type"] = "icon";
            value["columns"][1]["column"] = "sim_name";
            value["columns"][1]["value"] = info->getName();
            // </FS:Ansariel>
            mSearchResults->addElement(value);
            num_results++;
        }
    }

    if (found_null_sim)
    {
        mCompletingRegionName = "";
    }

    if (num_results > 0)
    {
        // <FS:Ansariel> Let's sort the list to make it more user-friendly
        mSearchResults->sortByColumn("sim_name", true);

        // if match found, highlight it and go
        if (!match.isUndefined())
        {
            mSearchResults->selectByValue(match);
            mSearchResults->setFocus(true);
            onCommitSearchResult(false /*fully commit the only option*/);
        }
        // else let user decide
        else
        {
            mSearchResults->selectFirstItem();
            mSearchResults->setFocus(true);
            onCommitSearchResult(true /*don't update text field*/);
        }
    }
    else
    {
        // if we found nothing, say "none"
        mProcessingSearchUpdate = false;
        mSearchResults->setCommentText(LLTrans::getString("worldmap_results_none_found"));
        mSearchResults->operateOnAll(LLCtrlListInterface::OP_DESELECT);
    }
}

void LLFloaterWorldMap::onTeleportFinished()
{
    if(isInVisibleChain())
    {
        mMapView->setPan(0, 0, true);
    }
}

void LLFloaterWorldMap::onCommitSearchResult(bool from_search)
{
    bool wolf_flying = false;   // <WolfViewer 2026-10-02/> the flight centres it instead
    std::string sim_name = mSearchResults->getSelectedValue().asString();
    if (sim_name.empty())
    {
        return;
    }
    LLStringUtil::toLower(sim_name);

    // [REGION CATEGORIES 2026-10-30] A category result comes from wolf-grid.com, not from the map: when the map does
    // not know the region yet, ask for its square (the MapBlockRequest a map pan sends) and come back here from
    // updateSims when it arrives (wolfCatResolvePending), so selecting it tracks it exactly like a name result.
    {
        if (!mWolfCatPending.empty() && mWolfCatPending != sim_name)
        {
            mWolfCatPending.clear();           // another row was chosen: forget the earlier lookup
            mWolfCatPendingTeleport = false;
        }
        const auto cat = mWolfCatCells.find(sim_name);
        if (cat != mWolfCatCells.end() && !LLWorldMap::getInstance()->simInfoFromName(sim_name))
        {
            mWolfCatPending = sim_name;
            mWolfCatPendingFromSearch = from_search;
            mWolfCatPendingAt = LLFrameTimer::getElapsedSeconds();
            LLWorldMapMessage::getInstance()->sendMapBlockRequest(cat->second.first, cat->second.second,
                                                                  cat->second.first, cat->second.second);
            return;
        }
    }

    LLWorldMap::sim_info_map_t::const_iterator end = LLWorldMap::instance().getRegionMap().end();
    for (LLWorldMap::sim_info_map_t::const_iterator it = LLWorldMap::getInstance()->getRegionMap().begin(); it != end; ++it)
    {
        LLSimInfo* info = it->second;

        if (info->isName(sim_name))
        {
            LLVector3d pos_global = info->getGlobalOrigin();

            constexpr F64 SIM_COORD_DEFAULT = 128.0;
            LLVector3 pos_local(SIM_COORD_DEFAULT, SIM_COORD_DEFAULT, 0.0f);
            // <FS:TJ> [FIRE-36896] Set the teleport coordinates if provided in OpenSim
        #ifdef OPENSIM
            if (LLGridManager::getInstance()->isInOpenSim() && LLUrlRegistry::instance().isUrl(sim_name))
            {
                LLSLURL slurl = LLSLURL(sim_name);
                if (slurl.getType() == LLSLURL::LOCATION)
                {
                    LLVector3 slurl_position = slurl.getPosition();
                    pos_local.set(slurl_position);
                }
            }
        #endif
            // </FS:TJ>

            // Did this value come from a trackURL() request?
            if (!mCompletingRegionPos.isExactlyZero())
            {
                pos_local = mCompletingRegionPos;
                mCompletingRegionPos.clear();
            }
            pos_global.mdV[VX] += (F64)pos_local.mV[VX];
            pos_global.mdV[VY] += (F64)pos_local.mV[VY];
            pos_global.mdV[VZ] = (F64)pos_local.mV[VZ];

            // Commiting search string automatically selects first item in the search list,
            // in such case onCommitSearchResult shouldn't modify search string
            if (!from_search)
            {
                mLocationEditor->setValue(sim_name);
            }
            trackLocation(pos_global);
            mProcessingSearchUpdate = from_search;
            mTrackCtrlsPanel->setDefaultBtn(mTeleportButton);
            // [REGION CATEGORIES PINS 2026-10-31] Paul: "clicking a region entry should zoom the map to that region".
            // A category row zooms to the whole region whether or not the globe is on (wolfShowArea: the globe's
            // flight, or the flat map's zoom and pan), and its pin becomes the chosen one.
            if (mWolfCatCells.count(sim_name))
            {
                mMapView->wolfSetCategoryPinSelected(mSearchResults->getSelectedValue().asString());
                wolfCatZoomToRegion(info);
                wolf_flying = true;
                break;
            }
            // <WolfViewer 2026-10-02> Wolf Territories: zoom down to the region found (from the
            // globe too), not only pan to it. onShowTargetBtn below does the pan.
            if (WolfMapGlobe::instance().active())
            {
                const LLRect view_rect = mMapView->getRect();
                // The middle of the region, not the landing point: the whole region is shown.
                LLVector3d middle = info->getGlobalOrigin();
                middle.mdV[VX] += info->mSizeX * 0.5;
                middle.mdV[VY] += info->mSizeY * 0.5;
                mMapView->wolfFlyTo(middle, WolfMapGlobe::instance().regionZoom((F32)info->mSizeX, (F32)info->mSizeY,
                                                                                view_rect.getWidth(), view_rect.getHeight()));
                wolf_flying = true;
            }
            // </WolfViewer>
            break;
        }
    }

    if (!wolf_flying)
    {
        onShowTargetBtn();
    }
}

void LLFloaterWorldMap::onChangeMaturity()
{
    bool can_access_mature = gAgent.canAccessMature();
    bool can_access_adult = gAgent.canAccessAdult();

    getChildView("events_mature_icon")->setVisible( can_access_mature);
    getChildView("events_mature_label")->setVisible( can_access_mature);
    getChildView("events_mature_chk")->setVisible( can_access_mature);

    getChildView("events_adult_icon")->setVisible( can_access_adult);
    getChildView("events_adult_label")->setVisible( can_access_adult);
    getChildView("events_adult_chk")->setVisible( can_access_adult);

    // disable mature / adult events.
    if (!can_access_mature)
    {
        gSavedSettings.setBOOL("ShowMatureEvents", false);
    }
    if (!can_access_adult)
    {
        gSavedSettings.setBOOL("ShowAdultEvents", false);
    }
}

void LLFloaterWorldMap::onFocusLost()
{
    gViewerWindow->showCursor();
    mMapView->mPanning = false;
}

LLPanelHideBeacon::LLPanelHideBeacon() :
    mHideButton(NULL)
{
}

// static
LLPanelHideBeacon* LLPanelHideBeacon::getInstance()
{
    static LLPanelHideBeacon* panel = getPanelHideBeacon();
    return panel;
}


bool LLPanelHideBeacon::postBuild()
{
    mHideButton = getChild<LLButton>("hide_beacon_btn");
    mHideButton->setCommitCallback(boost::bind(&LLPanelHideBeacon::onHideButtonClick, this));

    gViewerWindow->setOnWorldViewRectUpdated(boost::bind(&LLPanelHideBeacon::updatePosition, this));

    return true;
}

//virtual
void LLPanelHideBeacon::draw()
{
    if (!LLTracker::isTracking(NULL))
    {
        mHideButton->setVisible(false);
        return;
    }
    mHideButton->setVisible(true);
    updatePosition();
    LLPanel::draw();
}

//virtual
void LLPanelHideBeacon::setVisible(bool visible)
{
    if (gAgentCamera.getCameraMode() == CAMERA_MODE_MOUSELOOK) visible = false;

    if (visible)
    {
        updatePosition();
    }

    LLPanel::setVisible(visible);
}


//static
LLPanelHideBeacon* LLPanelHideBeacon::getPanelHideBeacon()
{
    LLPanelHideBeacon* panel = new LLPanelHideBeacon();
    panel->buildFromFile("panel_hide_beacon.xml");

    LL_INFOS() << "Build LLPanelHideBeacon panel" << LL_ENDL;

    panel->updatePosition();
    return panel;
}

void LLPanelHideBeacon::onHideButtonClick()
{
    LLFloaterWorldMap* instance = LLFloaterWorldMap::getInstance();
    if (instance)
    {
        instance->onClearBtn();
    }
}

/**
* Updates position of the panel (similar to Stand & Stop Flying panel).
*/
void LLPanelHideBeacon::updatePosition()
{
    S32 bottom_tb_center = 0;
    if (LLToolBar* toolbar_bottom = gToolBarView->getToolbar(LLToolBarEnums::TOOLBAR_BOTTOM))
    {
        bottom_tb_center = toolbar_bottom->getRect().getCenterX();
    }

    S32 left_tb_width = 0;
    if (LLToolBar* toolbar_left = gToolBarView->getToolbar(LLToolBarEnums::TOOLBAR_LEFT))
    {
        left_tb_width = toolbar_left->getRect().getWidth();
    }

    if (gToolBarView != NULL && gToolBarView->getToolbar(LLToolBarEnums::TOOLBAR_LEFT)->hasButtons())
    {
        S32 x_pos = bottom_tb_center - getRect().getWidth() / 2 - left_tb_width;
        setOrigin( x_pos + HIDE_BEACON_PAD, 0);
    }
    else
    {
        S32 x_pos = bottom_tb_center - getRect().getWidth() / 2;
        setOrigin( x_pos + HIDE_BEACON_PAD, 0);
    }
}
