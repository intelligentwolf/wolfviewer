/**
 * @file wolffloaterlifts.cpp
 * @brief WolfViewer: Cable Cars and Ski Lifts (ticket 691). Design: wolffloaterlifts.h.
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

#include "llviewerprecompiledheaders.h"

#include "wolffloaterlifts.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

#include "fscommon.h"
#include "fsnearbychathub.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llapp.h"
#include "llbutton.h"
#include "llchat.h"
#include "llcoros.h"
#include "lleventcoro.h"
#include "llfloaterreg.h"
#include "llfloatertools.h"
#include "llfocusmgr.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llnotificationsutil.h"
#include "llprogressbar.h"
#include "llradiogroup.h"
#include "llrender.h"
#include "llsurface.h"
#include "lltextbox.h"
#include "lltooldraganddrop.h"
#include "lltoolmgr.h"
#include "llviewercamera.h"
#include "llviewerinventory.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "message.h"
#include "lluicolortable.h"
#include "wolfcave.h"
#include "wolfgrid.h"

namespace
{
    // lift_setup.lsl VCH: the box's channel (ticket 691); negative = a ScriptDialogReply, heard region-wide
    constexpr S32 VCH = -76910;
    const std::string PREFIX = "WolfLift1|";
    // build_lifts.py BOX_DESC: the kit's description starts with this word (wolflifts_lib.php WLF_DESC_PREFIX)
    const std::string KIT_DESC = "wolflift-kit";
    // php/wolflifts.php: gives the kit to a Wolf resident who has none (the skis' give: wolffloaterskis.cpp GIVE_URL)
    const char* GIVE_URL = "https://wolfstorm.app/php/wolflifts.php";
    const char* GIVE_BODY = "{\"action\":\"give\"}";
    constexpr F32 GIVE_TIMEOUT_S = 30.f;
    constexpr S32 GIVE_WAIT_S = 30;            // after the grid's answer, the kit is looked for this long (the 30 one-second waits before)
    constexpr size_t MAX_POINTS = 12;          // lift_setup.lsl MAX_POINTS: bottom, up to 10 bends, top
    constexpr size_t MAX_LINE = 1023;          // llRegionSayTo's longest line (LSL_Api.cs llRegionSayTo)
    constexpr F32 SAME_CLICK_M = 1.f;          // the second press of a double-click (wolffloaterterraform.cpp SAME_CLICK_M)
    constexpr F64 PLAN_WAIT_S = 0.6;           // the plan is asked for this long after the drawing last changed
    constexpr F64 BUILD_QUIET_S = 60.0;        // no news from the box for this long while building: say so
    // [W104 FEATFIX 2026-11-11 A1] no news for this long: the window stops waiting. The box itself gives up after 30 s without
    // news (lift_setup.lsl timer(): llGetTime() - gLastNews > 30.0 -> failBuild), so by then its "fail" line was lost or it is gone.
    constexpr F64 BUILD_GIVE_UP_S = 120.0;
    // [W104 FEATFIX 2026-11-11 A5] after a failed kit fetch / rez the kit is asked for again only after this (CHOSEN), doubling
    // after each further failure up to BOX_RETRY_MAX_S, and only when the resident changes the drawing
    constexpr F64 BOX_RETRY_FIRST_S = 30.0;
    constexpr F64 BOX_RETRY_MAX_S = 300.0;
    constexpr F32 TOP_VIEW_M = 120.f;          // wolffloatertunnels.cpp TOP_VIEW_M's role: the lowest camera height
    constexpr F32 TOP_VIEW_TILT = 0.35f;       // and a slight tilt, so the hill's shape shows

    LLColor4 PREVIEW_CABLE(1.f, 0.85f, 0.35f, 1.f);
    LLColor4 PREVIEW_TOWER(0.95f, 0.55f, 0.2f, 1.f);
    LLColor4 PREVIEW_STATION(0.45f, 0.85f, 1.f, 1.f);

    bool onWolf() { return WolfGrid::isOnWolfTerritories(); }

    // An object item (not a link) whose description starts "wolflift-kit" (wolfvehiclerez.cpp VehicleItems, the same test).
    class KitItems : public LLInventoryCollectFunctor
    {
    public:
        bool operator()(LLInventoryCategory*, LLInventoryItem* item) override
        {
            if (!item || item->getIsLinkType() || item->getType() != LLAssetType::AT_OBJECT
                || item->getInventoryType() != LLInventoryType::IT_OBJECT) return false;
            std::string d = item->getDescription();
            LLStringUtil::trimHead(d);
            LLStringUtil::toLower(d);
            if (d.compare(0, KIT_DESC.size(), KIT_DESC) != 0) return false;
            return d.size() == KIT_DESC.size() || !(isalnum((unsigned char)d[KIT_DESC.size()]) || d[KIT_DESC.size()] == '_' || d[KIT_DESC.size()] == '-');
        }
    };

    // [LIFTFIND 2026-11-11] The same cause as the Greedy pieces (wolfgreedysetup.cpp [GREEDYFIND 2026-11-11], read it for the
    // source lines): every item read from the inventory cache at login is "not finished" (llinventorymodel.cpp loadFromFile,
    // LLViewerInventoryItem() + the non-virtual LLInventoryItem::fromLLSD; lltooldraganddrop.cpp willObjectAcceptInventory), and
    // a folder whose cached version matches the login skeleton is not fetched again (llinventorymodelbackgroundfetch.cpp
    // bulkFetch asks only for VERSION_UNKNOWN folders). So a kit given in an earlier session was never seen by findKit (finished
    // items only), the grid answered {have: true} with no folder id (wolflifts_lib.php wlf_give), the 30 s wait fetched nothing
    // and the window said "could not see it yet". Now: an unfinished copy is asked for as a drag does (lltooldraganddrop.cpp
    // beginDrag: LLInventoryModelBackgroundFetch::start(item, false)), the kit's folders are asked for again, the grid is asked
    // only after the login's inventory fetch is done, and after its answer Objects, the kit's folder and the kit folders are
    // asked for again while the kit is looked for. Source: wolfgreedysetup.cpp findPieceIn / backgroundFetchIdle / greedyFolders
    // / refetchFolder / PieceSearch / putCoro step 2, the same steps for the lift kit.

    /** The resident's kit under `folder` (outside the Trash): the first one that is finished. A copy that is not finished yet
     *  (read from the inventory cache) goes into `unfinished`, when given, so that it can be asked for. */
    LLViewerInventoryItem* findKitIn(const LLUUID& folder, uuid_vec_t* unfinished)
    {
        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t items;
        KitItems match;
        gInventory.collectDescendentsIf(folder, cats, items, LLInventoryModel::EXCLUDE_TRASH, match);
        LLViewerInventoryItem* found = nullptr;
        for (const LLPointer<LLViewerInventoryItem>& p : items)
        {
            if (p.isNull()) continue;
            if (p->isFinished())
            {
                if (!found) found = p.get();
            }
            else if (unfinished)
            {
                unfinished->push_back(p->getUUID());
            }
        }
        return found;
    }

    /** The resident's kit anywhere in the inventory (outside the Trash): wolflifts_lib.php wlf_candidates counts a kit in any
     *  folder but the Trash. */
    LLViewerInventoryItem* findKit(uuid_vec_t* unfinished = nullptr)
    {
        return findKitIn(gInventory.getRootFolderID(), unfinished);
    }

    /** The login's background inventory fetch has nothing left to do. Source: wolfgreedysetup.cpp backgroundFetchIdle
     *  (llinventorymodelbackgroundfetch.cpp setAllFoldersFetched, isBulkFetchProcessingComplete, inventoryFetchStarted). */
    bool backgroundFetchIdle()
    {
        const LLInventoryModelBackgroundFetch& f = LLInventoryModelBackgroundFetch::instance();
        return f.inventoryFetchStarted() && !f.folderFetchActive() && f.isBulkFetchProcessingComplete();
    }

    /** Objects. Source: wolfgreedysetup.cpp objectsFolder (findUserDefinedCategoryUUIDForType FT_OBJECT). */
    LLUUID objectsFolder() { return gInventory.findUserDefinedCategoryUUIDForType(LLFolderType::FT_OBJECT); }

    // wolflifts_items.php 'folder' (build_lifts.py FOLDER_NAME): the grid gives the kit into a folder of this name directly
    // under Objects (wolflifts_lib.php wlf_give -> wolfskis_lib.php wsk_pick_folder / wsk_find_folder, an exact name match;
    // wlf_items 'old_folders' is empty)
    const std::string KIT_FOLDER = "Cable Cars and Ski Lifts";

    /** The folders directly under Objects named KIT_FOLDER. Source: wolfgreedysetup.cpp greedyFolders. */
    std::vector<LLUUID> kitFolders()
    {
        std::vector<LLUUID> out;
        const LLUUID objects = objectsFolder();
        if (objects.isNull()) return out;
        LLInventoryModel::cat_array_t* cats = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(objects, cats, items);
        if (!cats) return out;
        for (const LLPointer<LLViewerInventoryCategory>& c : *cats)
        {
            if (c.notNull() && c->getName() == KIT_FOLDER) out.push_back(c->getUUID());
        }
        return out;
    }

    /** Ask the region for a folder's contents again (once per folder per round: `asked`). False: no folder id yet, or the
     *  viewer does not know the folder (yet). Source: wolfgreedysetup.cpp refetchFolder. */
    bool refetchFolder(const LLUUID& id, std::vector<LLUUID>& asked, const char* what)
    {
        if (id.isNull()) return false;
        if (std::find(asked.begin(), asked.end(), id) != asked.end()) return true;
        LLViewerInventoryCategory* cat = gInventory.getCategory(id);
        if (!cat)
        {
            LL_INFOS("WolfLifts") << "kit search: " << what << " folder " << id << " is not known to this viewer yet" << LL_ENDL;
            return false;
        }
        asked.push_back(id);
        cat->setVersion(LLViewerInventoryCategory::VERSION_UNKNOWN);
        cat->fetch();
        LL_INFOS("WolfLifts") << "kit search: asked again for the " << what << " folder '" << cat->getName() << "' " << id << LL_ENDL;
        return true;
    }

    /** The kit search's scans while it waits (Source: wolfgreedysetup.cpp PieceSearch): the kit folders every time, the whole
     *  inventory at most every WHOLE_SCAN_S. Every unfinished copy either scan meets is asked for, once per round (again()). */
    class KitSearch
    {
    public:
        KitSearch() = default;
        KitSearch(const KitSearch&) = delete;
        KitSearch& operator=(const KitSearch&) = delete;

        LLViewerInventoryItem* find()
        {
            uuid_vec_t unfinished;
            LLViewerInventoryItem* got = nullptr;
            const F64 t = LLFrameTimer::getTotalSeconds();
            if (mWholeAt < 0.0 || t - mWholeAt >= WHOLE_SCAN_S)
            {
                mWholeAt = t;
                got = findKit(&unfinished);
            }
            else
            {
                for (const LLUUID& f : kitFolders())
                {
                    if ((got = findKitIn(f, &unfinished))) break;
                }
            }
            for (const LLUUID& id : unfinished)
            {
                if (std::find(mAsked.begin(), mAsked.end(), id) != mAsked.end()) continue;
                mAsked.push_back(id);
                LLInventoryModelBackgroundFetch::instance().start(id, false);
                LL_INFOS("WolfLifts") << "kit search: asked for the lift kit " << id << " (read from the inventory cache, not complete yet)" << LL_ENDL;
            }
            return got;
        }

        /** The next round asks for the unfinished copies again. */
        void again() { mAsked.clear(); }

        /** How many unfinished copies have been asked for this round. */
        size_t askedCount() const { return mAsked.size(); }

    private:
        static constexpr F64 WHOLE_SCAN_S = 5.0;   // CHOSEN (wolfgreedysetup.cpp PieceSearch WHOLE_SCAN_S)
        uuid_vec_t mAsked;
        F64 mWholeAt = -1.0;
    };

    /** Rez `item` with its root at `at` (region metres) in the agent's region. Source: wolffloatertunnels.cpp rezTunnel
     *  (wolfcave.cpp WolfCaves::rezExact; lltooldraganddrop.cpp:1970-2028 dropObject; RayEndIsIntersection = 1 places the
     *  object at RayEnd, Scene.cs:2697-2706; RayStart = RayEnd keeps the water test from moving it). */
    void rezAt(LLViewerInventoryItem* item, const LLVector3& at)
    {
        LLViewerRegion* rgn = gAgent.getRegion();
        if (!rgn || !item) return;
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_RezObject);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->addUUIDFast(_PREHASH_GroupID, FSCommon::getGroupForRezzing());
        msg->nextBlock("RezData");
        msg->addUUIDFast(_PREHASH_FromTaskID, LLUUID::null);
        msg->addU8Fast(_PREHASH_BypassRaycast, (U8)1);
        msg->addVector3Fast(_PREHASH_RayStart, at);
        msg->addVector3Fast(_PREHASH_RayEnd, at);
        msg->addUUIDFast(_PREHASH_RayTargetID, LLUUID::null);
        msg->addBOOLFast(_PREHASH_RayEndIsIntersection, true);
        msg->addBOOLFast(_PREHASH_RezSelected, false);
        msg->addBOOLFast(_PREHASH_RemoveItem, false);
        pack_permissions_slam(msg, item->getFlags(), item->getPermissions());
        msg->nextBlockFast(_PREHASH_InventoryData);
        item->packMessage(msg);
        msg->sendReliable(rgn->getHost());
        FSCommon::sObjectAddMsg = 0;   // lltooldraganddrop.cpp:2030-2032: no build defaults for it
    }

    // "x,y" -> the two numbers; false when it is not that
    bool parseXY(const std::string& s, F32 out[], size_t n)
    {
        std::vector<std::string> parts;
        LLStringUtil::getTokens(s, parts, ",");
        if (parts.size() != n) return false;
        for (size_t i = 0; i < n; ++i)
        {
            char* end = nullptr;
            const double v = strtod(parts[i].c_str(), &end);
            if (!end || *end != '\0' || !std::isfinite(v)) return false;
            out[i] = (F32)v;
        }
        return true;
    }

    // "a|b|c" -> its fields, empty ones kept (llParseStringKeepNulls)
    std::vector<std::string> fields(const std::string& s)
    {
        std::vector<std::string> out;
        size_t at = 0;
        while (true)
        {
            const size_t bar = s.find('|', at);
            out.push_back(s.substr(at, bar == std::string::npos ? std::string::npos : bar - at));
            if (bar == std::string::npos) break;
            at = bar + 1;
        }
        return out;
    }
}

bool WolfLifts::onObjectChat(const LLUUID& source, const LLUUID& owner, U8 chat_type, const std::string& text)
{
    if (text.compare(0, PREFIX.size(), PREFIX) != 0) return false;
    // [WOLF ONLY] as WolfAir::onObjectChat: off Wolf the line is left to chat like any other
    if (!onWolf() || chat_type != CHAT_TYPE_DIRECT) return false;
    // [W104 FEATFIX 2026-11-11 A2] only an object this agent owns is the kit (lift_setup.lsl listens to and answers only its
    // owner): the same test as WolfFishing::onObjectChat - the region writes the OwnerID. Anyone else's line is left to chat.
    if (source.isNull() || owner != gAgent.getID()) return false;
    // from here the line is the window's, never chat (the box only answers this window)
    if (text.size() > MAX_LINE) return true;
    if (WolfFloaterLifts* f = LLFloaterReg::findTypedInstance<WolfFloaterLifts>("wolf_lifts")) f->onBoxLine(source, text);
    return true;
}

WolfFloaterLifts::WolfFloaterLifts(const LLSD& key) : LLFloater(key) {}

WolfFloaterLifts::~WolfFloaterLifts()
{
    if (WolfToolTunnel::instanceExists() && WolfToolTunnel::getInstance()->hostIs(this)) WolfToolTunnel::getInstance()->detach();
}

bool WolfFloaterLifts::postBuild()
{
    mHint = getChild<LLTextBox>("lifts_hint");
    mKind = getChild<LLRadioGroup>("lifts_kind");
    mUndoBtn = getChild<LLButton>("lifts_undo");
    mStartOverBtn = getChild<LLButton>("lifts_start_over");
    mTopViewBtn = getChild<LLButton>("lifts_top_view");
    mPlanInfo = getChild<LLTextBox>("lifts_plan");
    mBuildBtn = getChild<LLButton>("lifts_build");
    mProgress = getChild<LLProgressBar>("lifts_progress");
    mStatus = getChild<LLTextBox>("lifts_status");
    mKind->setCommitCallback([this](LLUICtrl*, const LLSD&) { changed(); });
    mUndoBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { undoPoint(); });
    mStartOverBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { startOver(); });
    mTopViewBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { lookFromAbove(); });
    mBuildBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onBuild(); });
    mProgress->setVisible(false);
    mKind->setSelectedIndex(0);   // Cable car until the resident picks
    return true;
}

void WolfFloaterLifts::onOpen(const LLSD& key)
{
    if (!onWolf())
    {
        LLNotificationsUtil::add("GenericAlertOK", LLSD().with("MESSAGE",
            std::string("Cable Cars and Ski Lifts is for Wolf Territories regions: the lifts run on the Wolf grid's scripts.")));
        closeFloater();
        return;
    }
    // one pen: the Tunnels and Terraform windows draw with it too (wolffloaterterraform.cpp onOpen, the same)
    for (const char* other : { "wolf_tunnels", "wolf_terraform" })
    {
        if (LLFloaterReg::instanceVisible(other))
        {
            LLFloaterReg::hideInstance(other);
            setStatus("The Tunnels / Terraform window was closed: it draws with the same pen.", false);
        }
    }
    mStartPending = true;
    // [W104 FEATFIX 2026-11-11 A1/A5] a fresh start: nothing is waited for from before, and a kit that failed is tried again
    // (the failure messages say "close the window and open it again")
    if (mBuilding) endBuilding(std::string(), false);
    mBoxRetryAt = 0.0;
    mBoxBackoff = 0.0;
    mBoxAlerted = false;
    if (!mOpenNote.empty())
    {
        setStatus(mOpenNote, false);
        mOpenNote.clear();
    }
    if (mClicks.size() >= 2) changed();   // the plan is asked for again (the box was let go on close)
    updateControls();
}

void WolfFloaterLifts::onClose(bool app_quitting)
{
    if (WolfToolTunnel::instanceExists() && WolfToolTunnel::getInstance()->hostIs(this)) WolfToolTunnel::getInstance()->detach();
    mPenWanted = false;
    if (app_quitting) return;
    // a box this window found but nothing was built with: it tidies itself away (lift_setup.lsl "bye")
    if (mBox == BOX_READY && !mBuilding) say("WLIFT|" + mBox8 + "|bye");
    // [W104 FEATFIX 2026-11-11 A5] one this window rezzed that has not answered yet: told the same (its box8 is the first 8
    // letters of its key, lift_setup.lsl state_entry gBox8). If it was not listening yet it stays, and the next window finds it.
    else if (mBox == BOX_FINDING && mWaitFor.notNull()) say("WLIFT|" + mWaitFor.asString().substr(0, 8) + "|bye");
    // [W104 FEATFIX 2026-11-11 A1] closed mid-build: the kit builds on by itself (and takes the parts away if it fails); this
    // window stops waiting, so it never stays locked, and says so when it is opened again. The drawing went into the build.
    if (mBuilding)
    {
        endBuilding(std::string(), false);
        mClicks.clear();
        mPlanText.clear();
        mOpenNote = "The window was closed while your lift was being built. The lift kit carries on by itself and tidies itself "
                    "away when it has finished - look for the lift in the world.";
    }
    if (mBoxCoroRunning) LL_INFOS("WolfLifts") << "window closed while getting the kit ready: that stops" << LL_ENDL;
    stopBox();
    mPlan.clear();
    mPlanOk = false;
    giveCameraBack();
}

// ---- the drawing ----------------------------------------------------------------------------------------------------------------

bool WolfFloaterLifts::routeEditing() const
{
    return getVisible() && !isMinimized() && onWolf() && !mBuilding;
}

void WolfFloaterLifts::clickAt(F32 x, F32 y)
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn || mBuilding) return;
    if (mRegionAt != rgn->getRegionID()) { mClicks.clear(); mRegionAt = rgn->getRegionID(); }
    const LLVector2 p(x, y);
    if (!mClicks.empty() && dist_vec(mClicks.back(), p) < SAME_CLICK_M) return;   // the second press of a double-click
    if (mClicks.size() >= MAX_POINTS)
    {
        setStatus("That is the most points one lift can have - drag a point to move it, or press Undo last point.", true);
        return;
    }
    mClicks.push_back(p);
    setStatus(std::string(), false);
    changed();
}

void WolfFloaterLifts::moveClick(S32 k, F32 x, F32 y, bool done)
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn || mBuilding || k < 0 || (size_t)k >= mClicks.size()) return;
    const F32 w = rgn->getWidth();
    mClicks[k].set(llclamp(x, 0.f, w - 1.f), llclamp(y, 0.f, w - 1.f));
    changed();
    (void)done;
}

void WolfFloaterLifts::undoPoint()
{
    if (mClicks.empty() || mBuilding) return;
    mClicks.pop_back();
    setStatus(mClicks.empty() ? std::string("No points left - click where the bottom station goes.") : std::string("The last point was taken back."), false);
    changed();
}

void WolfFloaterLifts::removeClick(S32 k)
{
    if (mBuilding || k < 0 || (size_t)k >= mClicks.size()) return;
    if (mClicks.size() <= 2)
    {
        setStatus("A lift needs both its stations - drag them instead, or press Undo last point.", true);
        return;
    }
    mClicks.erase(mClicks.begin() + k);
    setStatus(llformat("Point %d was taken out.", k + 1), false);
    changed();
}

// Source: wolffloaterterraform.cpp WolfFloaterTerraform::insertClickNear (the piece of the line nearest the click gets the point).
void WolfFloaterLifts::insertClickNear(F32 x, F32 y)
{
    if (mBuilding) return;
    if (mClicks.size() < 2) { clickAt(x, y); return; }
    if (mClicks.size() >= MAX_POINTS) { setStatus("That is the most bends one lift can have.", true); return; }
    size_t best = 1;
    F32 best_d = 1e30f;
    const LLVector2 q(x, y);
    for (size_t i = 1; i < mClicks.size(); ++i)
    {
        const LLVector2 a = mClicks[i - 1], v = mClicks[i] - mClicks[i - 1];
        const F32 l2 = v.lengthSquared();
        const F32 u = l2 > 1e-9f ? llclamp(((q - a) * v) / l2, 0.f, 1.f) : 0.f;
        const F32 d = dist_vec(a + v * u, q);
        if (d < best_d) { best_d = d; best = i; }
    }
    if (best_d > 6.f) { setStatus("Ctrl+click on the line to put a bend there.", true); return; }
    if (dist_vec(q, mClicks[best - 1]) < SAME_CLICK_M || dist_vec(q, mClicks[best]) < SAME_CLICK_M) return;
    mClicks.insert(mClicks.begin() + best, q);
    setStatus("A bend was put in - drag it where you want it.", false);
    changed();
}

void WolfFloaterLifts::setStatus(const std::string& text, bool error)
{
    if (!mStatus) return;
    mStatus->setText(text);
    mStatus->setColor(LLUIColorTable::instance().getColor(error ? "AlertCautionTextColor" : "LabelTextColor"));
}

void WolfFloaterLifts::penStopped()
{
    updateControls();
}

// Source: wolffloaterterraform.cpp WolfFloaterTerraform::planeHeight (from under the ground a click lands on a level plane).
F32 WolfFloaterLifts::planeHeight() const
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return 0.f;
    if (!mClicks.empty()) return rgn->getLand().resolveHeightRegion(mClicks.back().mV[0], mClicks.back().mV[1]);
    return rgn->getPosRegionFromAgent(gAgent.getPositionAgent()).mV[VZ] - 1.f;
}

std::string WolfFloaterLifts::noGroundText(bool underground) const
{
    return underground ? "Point at the ground inside this region (you are under the ground)."
                       : "Click on the ground of the region you are in - a lift stays inside one region.";
}

std::string WolfFloaterLifts::escapeText() const
{
    return "You are laying out a lift. Backspace takes the last point back; closing the window stops.";
}

std::string WolfFloaterLifts::markerLabel(size_t k) const
{
    if (k == 0) return "Bottom station";
    if (mClicks.size() >= 2 && k + 1 == mClicks.size()) return "Top station";
    return "Bend";
}

std::string WolfFloaterLifts::routeHint() const
{
    if (mBuilding) return "Building your lift - the parts are being put in place.";
    if (mClicks.empty()) return "Click on the ground where the BOTTOM station goes.";
    if (mClicks.size() == 1) return "Now click where the TOP station goes (to bend the line, click the bend first). Too far to click? Alt + drag moves the camera.";
    // [W104 FEATFIX 2026-11-11 A5] after a failure the kit is tried again when the drawing changes, after a short wait
    if (mBox == BOX_NONE && mBoxBackoff > 0.0)
    {
        return LLFrameTimer::getElapsedSeconds() < mBoxRetryAt
            ? "Your lift kit is not ready - the window says why. Wait a little, then move a point to try again."
            : "Move a point to try getting your lift kit ready again.";
    }
    if (mBox != BOX_READY) return "Getting your lift kit ready...";
    if (mPlanDirty) return "Working out where the towers go...";
    if (!mPlanOk) return "This line cannot be built yet - see the window. Drag a point to move it.";
    return "Press Build in the window. Drag a point to move it; Ctrl+click on the line adds a bend.";
}

void WolfFloaterLifts::renderRouteExtras(LLViewerRegion* rgn)
{
    if (!rgn || !getVisible() || isMinimized() || !mPlanOk || mPlanDirty || rgn->getRegionID() != mRegionAt || mPlan.size() < 2) return;
    auto A = [&](const LLVector3& p) { return rgn->getPosAgentFromRegion(p); };
    LLGLDepthTest depth(GL_FALSE);
    // the cable, from support to support
    gGL.setLineWidth(3.f);
    gGL.color4fv(PREVIEW_CABLE.mV);
    gGL.begin(LLRender::LINES);
    for (size_t i = 1; i < mPlan.size(); ++i)
    {
        gGL.vertex3fv(A(mPlan[i - 1].mCable).mV);
        gGL.vertex3fv(A(mPlan[i].mCable).mV);
    }
    gGL.end();
    // the towers (ground to cable) and the stations (a square on the ground and a post up to the cable)
    gGL.setLineWidth(4.f);
    gGL.begin(LLRender::LINES);
    for (const Support& s : mPlan)
    {
        const LLVector3 foot(s.mCable.mV[VX], s.mCable.mV[VY], s.mGround);
        gGL.color4fv((s.mTower ? PREVIEW_TOWER : PREVIEW_STATION).mV);
        gGL.vertex3fv(A(foot).mV);
        gGL.vertex3fv(A(s.mCable).mV);
        if (!s.mTower)
        {
            const F32 h = 4.f;
            const LLVector3 c[4] = { foot + LLVector3(-h, -h, 0.3f), foot + LLVector3(h, -h, 0.3f), foot + LLVector3(h, h, 0.3f), foot + LLVector3(-h, h, 0.3f) };
            for (S32 k = 0; k < 4; ++k) { gGL.vertex3fv(A(c[k]).mV); gGL.vertex3fv(A(c[(k + 1) % 4]).mV); }
        }
    }
    gGL.end();
    gGL.setLineWidth(1.f);
}

void WolfFloaterLifts::changed()
{
    mPlanDirty = true;
    mPlanOk = false;
    mChangedAt = LLFrameTimer::getElapsedSeconds();
    if (mClicks.size() >= 2 && mBox == BOX_NONE) needBox();
    updateControls();
}

std::string WolfFloaterLifts::kindWord() const
{
    return mKind && mKind->getSelectedIndex() == 1 ? "chair" : "car";
}

std::string WolfFloaterLifts::pointsText() const
{
    std::string s;
    for (const LLVector2& p : mClicks)
    {
        if (!s.empty()) s += ";";
        s += llformat("%.1f,%.1f", p.mV[0], p.mV[1]);
    }
    return s;
}

void WolfFloaterLifts::say(const std::string& text)
{
    // a negative channel goes as a ScriptDialogReply, heard region-wide (fsnearbychathub.cpp sendChatFromViewerFinal);
    // its ButtonLabel is Variable 1 (message_template.msg ScriptDialogReply): at most 254 letters and the ending nul
    if (text.size() > 254) { LL_WARNS("WolfLifts") << "line too long for the box: " << text.size() << LL_ENDL; return; }
    FSNearbyChat::sendChatFromViewerFinal(text, text, CHAT_TYPE_NORMAL, false, VCH);
}

void WolfFloaterLifts::sendPlan()
{
    mPlanDirty = false;
    if (mClicks.size() < 2 || mBox != BOX_READY) return;
    ++mPlanReq;
    say("WLIFT|" + mBox8 + "|plan|" + llformat("%d", mPlanReq) + "|" + kindWord() + "|" + pointsText());
}

void WolfFloaterLifts::onBuild()
{
    if (mBuilding || !mPlanOk || mPlanDirty || mBox != BOX_READY || mClicks.size() < 2) return;
    mBuilding = true;
    mBuildNewsAt = LLFrameTimer::getElapsedSeconds();
    mQuietSaid = false;
    mProgress->setValue(0.f);
    mProgress->setVisible(true);
    say("WLIFT|" + mBox8 + "|build|" + kindWord() + "|" + pointsText());
    setStatus("Building your " + std::string(kindWord() == "chair" ? "chair lift" : "cable car") + " - the parts are being put in place.", false);
    updateControls();
}

void WolfFloaterLifts::startOver()
{
    if (mBuilding) return;
    mClicks.clear();
    mPlan.clear();
    mPlanOk = false;
    mPlanText.clear();
    setStatus("Click on the ground where the bottom station goes.", false);
    changed();
}

void WolfFloaterLifts::onBoxLine(const LLUUID& source, const std::string& text)
{
    const std::vector<std::string> f = fields(text);
    if (f.size() < 3) return;
    const std::string& what = f[1];
    if (what == "hi")
    {
        // WolfLift1|hi|<box key>|<busy>: the box this window rezzed, or (none rezzed) any of ours that is not building
        if (mBox == BOX_READY || f.size() < 4) return;
        LLUUID id;
        if (!LLUUID::validate(f[2]) || !id.set(f[2]) || id != source) return;
        if (mWaitFor.notNull() && id != mWaitFor) return;
        if (f[3] != "0")
        {
            // [W104 FEATFIX 2026-11-11 A1] a kit of ours still building a lift (this window was closed or left the region while
            // it built): no second kit is put down beside it (boxCoro reads this after its hello)
            if (mBox == BOX_FINDING) mBusyKitSeen = true;
            return;
        }
        mBox = BOX_READY;
        mBoxId = id;
        mBox8 = f[2].substr(0, 8);
        mBoxBackoff = 0.0;      // [A5] working again: a later failure starts from the first wait, with its alert
        mBoxRetryAt = 0.0;
        mBoxAlerted = false;
        setStatus(std::string(), false);
        sendPlan();
        updateControls();
        return;
    }
    if (mBox != BOX_READY || source != mBoxId || f[2] != mBox8) return;
    if (what == "plan" && f.size() >= 6)
    {
        // WolfLift1|plan|<box8>|<req>|ok|<m>|<towers>|<rise>|x,y,z,g,t;...|<note>   or   ...|<req>|no|<why>
        if (atoi(f[3].c_str()) != mPlanReq || mPlanDirty) return;   // an older drawing's answer
        mPlan.clear();
        if (f[4] == "ok" && f.size() >= 9)
        {
            std::vector<std::string> sup;
            LLStringUtil::getTokens(f[8], sup, ";");
            for (const std::string& s : sup)
            {
                F32 v[5];
                if (!parseXY(s, v, 5)) { mPlan.clear(); break; }
                Support p;
                p.mCable.set(v[0], v[1], v[2]);
                p.mGround = v[3];
                p.mTower = v[4] > 0.5f;
                mPlan.push_back(p);
            }
            mPlanOk = mPlan.size() >= 2;
            const S32 towers = atoi(f[6].c_str()), rise = atoi(f[7].c_str());
            mPlanText = llformat("%s m long, %d tower%s", f[5].c_str(), towers, towers == 1 ? "" : "s");
            if (rise > 0) mPlanText += llformat(", %d m up", rise);
            else if (rise < 0) mPlanText += llformat(", %d m down", -rise);
            mPlanText += ".";
            if (f.size() >= 10 && !f[9].empty()) mPlanText += " " + f[9];
            mPlanInfo->setColor(LLUIColorTable::instance().getColor("LabelTextColor"));
        }
        else
        {
            mPlanOk = false;
            mPlanText = f.size() >= 6 ? f[5] : std::string("This line cannot be built.");
            mPlanInfo->setColor(LLUIColorTable::instance().getColor("AlertCautionTextColor"));
        }
        updateControls();
        return;
    }
    if (what == "step" && f.size() >= 5)
    {
        const F32 placed = (F32)atoi(f[3].c_str()), total = (F32)llmax(1, atoi(f[4].c_str()));
        mProgress->setValue(100.f * placed / total);
        mBuildNewsAt = LLFrameTimer::getElapsedSeconds();
        mQuietSaid = false;
        setStatus(llformat("Building: %d of %d parts in place.", (S32)placed, (S32)total), false);
        return;
    }
    if (what == "built" || what == "fail")
    {
        const std::string said = f.size() >= 4 ? f[3] : std::string();
        const bool built = what == "built";
        mBuilding = false;
        mProgress->setVisible(false);
        if (built)
        {
            // the box has tidied itself away: the next lift asks for a new one
            mBox = BOX_NONE;
            mBoxId.setNull();
            mWaitFor.setNull();
            mClicks.clear();
            mPlan.clear();
            mPlanOk = false;
            mPlanText.clear();
            setStatus("Built! " + said + " To lay out another, click where its bottom station goes.", false);
        }
        else
        {
            setStatus("The lift was not built: " + said, true);
            // a failure a resident must be able to read and act on: an alert stays until OK is pressed
            LLNotificationsUtil::add("GenericAlertOK", LLSD().with("MESSAGE", "The lift was not built.\n\n" + said));
            changed();
        }
        updateControls();
    }
}

// ---- the set up box ----------------------------------------------------------------------------------------------------------

void WolfFloaterLifts::needBox()
{
    if (mBoxCoroRunning || mBox != BOX_NONE) return;
    // [W104 FEATFIX 2026-11-11 A5] after a failure, not again before its wait is over (the status line still says why): a drag
    // or a click no longer sets off a fetch and an alert each time
    if (LLFrameTimer::getElapsedSeconds() < mBoxRetryAt) return;
    mBox = BOX_FINDING;
    mBoxCoroRunning = true;
    mBusyKitSeen = false;
    mWaitFor.setNull();
    LLHandle<LLFloater> h = getHandle();
    const U32 gen = mBoxGen;
    LLCoros::instance().launch("WolfLiftsBox", [h, gen]() { boxCoro(h, gen); });
}

// [W104 FEATFIX 2026-11-11 A5] wolffloaterlifts.h
void WolfFloaterLifts::stopBox()
{
    ++mBoxGen;
    mBoxCoroRunning = false;
    mBox = BOX_NONE;
    mBoxId.setNull();
    mWaitFor.setNull();
}

void WolfFloaterLifts::boxFailed(const std::string& why)
{
    mBox = BOX_NONE;
    setStatus(why, true);
    // [W104 FEATFIX 2026-11-11 A5] one alert for a run of failures (it stays until OK is pressed); the status line says each one
    mBoxBackoff = mBoxBackoff <= 0.0 ? BOX_RETRY_FIRST_S : llmin(mBoxBackoff * 2.0, BOX_RETRY_MAX_S);
    mBoxRetryAt = LLFrameTimer::getElapsedSeconds() + mBoxBackoff;
    if (!mBoxAlerted)
    {
        mBoxAlerted = true;
        LLNotificationsUtil::add("GenericAlertOK", LLSD().with("MESSAGE", why));
    }
    updateControls();
}

// [W104 FEATFIX 2026-11-11 A1] wolffloaterlifts.h
void WolfFloaterLifts::endBuilding(const std::string& why, bool error)
{
    mBuilding = false;
    if (mProgress) mProgress->setVisible(false);
    stopBox();   // the kit is let go: the next plan finds it again (or another)
    mPlan.clear();
    mPlanOk = false;
    if (!why.empty()) setStatus(why, error);
    updateControls();
}

// static
void WolfFloaterLifts::boxCoro(LLHandle<LLFloater> h, U32 gen)
{
    // The window is looked up again through its handle after every wait (wolffloaterskis.cpp giveCoro): closed meanwhile,
    // the coroutine stops. [W104 FEATFIX 2026-11-11 A5] The window is kept when closed (single_instance floaters are reused,
    // llfloater.cpp mReuseInstance), so closing (or a region change) bumps mBoxGen and a coroutine of an older generation
    // stops at its next wake-up: no kit is fetched or rezzed for a closed window.
    auto self = [&h, gen]() -> WolfFloaterLifts*
    {
        WolfFloaterLifts* f = dynamic_cast<WolfFloaterLifts*>(h.get());
        return f && f->mBoxGen == gen ? f : nullptr;
    };
    auto done = [&self]() { if (WolfFloaterLifts* f = self()) f->mBoxCoroRunning = false; };
    // 1. a box of ours already in this region (left from before, or rezzed by hand) answers hello
    if (WolfFloaterLifts* f = self()) f->say("WLIFT|*|hello");
    llcoro::suspendUntilTimeout(2.5f);
    WolfFloaterLifts* f = self();
    if (!f) return;
    if (f->mBox == BOX_READY || !f->getVisible()) { done(); return; }
    if (f->mBusyKitSeen)
    {
        // [W104 FEATFIX 2026-11-11 A1] a kit of ours is still building the last lift: wait for it rather than put down another
        f->mBoxCoroRunning = false;
        f->boxFailed("Your lift kit is still building your last lift. When it has finished, move a point to carry on.");
        return;
    }
    // 2. the kit in the inventory; when there is none, the grid gives one (php/wolflifts.php) and it is waited for.
    // [LIFTFIND 2026-11-11] never on a half-loaded inventory and never on copies read from the cache only (see findKitIn): the
    // login's background fetch finishes (or goes idle) first, the kit folders and any unfinished copy are asked for, and only
    // then the grid (Source: wolfgreedysetup.cpp putCoro step 2, the same waits)
    KitSearch search;
    LLViewerInventoryItem* kit = search.find();
    if (!kit)
    {
        std::vector<LLUUID> asked;
        LL_INFOS("WolfLifts") << "kit search: no complete lift kit in the loaded inventory (" << search.askedCount()
                              << " copies from the inventory cache asked for); background fetch "
                              << (LLInventoryModelBackgroundFetch::instance().isEverythingFetched() ? "complete"
                                  : (backgroundFetchIdle() ? "idle" : "still running"))
                              << ", Objects " << objectsFolder() << ", kit folders " << kitFolders().size() << LL_ENDL;
        f->setStatus("Looking for your lift kit in your inventory...", false);
        for (const LLUUID& k : kitFolders()) refetchFolder(k, asked, "kit");
        // CHOSEN (wolfgreedysetup.cpp putCoro): up to 60 s for the inventory to finish loading (less once the fetch has nothing
        // left to do), then at least 6 s for the kit folders and the asked-for copies to come back
        for (S32 i = 0; i < 66 && !kit; ++i)
        {
            const bool loaded = LLInventoryModelBackgroundFetch::instance().isEverythingFetched();
            if ((loaded || backgroundFetchIdle()) && i >= 6)
            {
                LL_INFOS("WolfLifts") << "kit search: background fetch " << (loaded ? "complete" : "idle") << " after " << i << " s" << LL_ENDL;
                break;
            }
            llcoro::suspendUntilTimeout(1.f);
            if (!self()) return;
            for (const LLUUID& k : kitFolders()) refetchFolder(k, asked, "kit");
            kit = search.find();
        }
        if (!kit) kit = findKit();   // one whole scan before the grid is asked
        f = self();
        if (!f) return;
        LL_INFOS("WolfLifts") << "kit search: " << (kit ? "found " + kit->getUUID().asString() + " after the folders came back"
                                                      : std::string("still none - asking the grid")) << LL_ENDL;
        if (!kit)
        {
            // the waits above can be long: off Wolf Territories meanwhile, no kit is asked for
            if (!onWolf())
            {
                f->mBoxCoroRunning = false;
                f->boxFailed("Cable Cars and Ski Lifts works on Wolf Territories regions only.");
                return;
            }
            f->setStatus("You have no lift kit yet - getting one for you (it goes into Objects > Cable Cars and Ski Lifts)...", false);
            const std::string body(GIVE_BODY);
            LLSD result = WolfCaves::serviceRequest(GIVE_URL, &body, GIVE_TIMEOUT_S);
            LLSD reply;
            std::string transport;
            const S32 status = WolfCaves::serviceReply(result, reply, transport);
            f = self();
            if (!f) return;
            if (status != 200 || !reply.isMap() || !reply["success"].asBoolean())
            {
                std::string why = reply.isMap() && reply.has("error") ? reply["error"].asString()
                    : (transport.empty() ? llformat("the grid answered HTTP %d", status) : transport);
                f->mBoxCoroRunning = false;
                f->boxFailed("The lift kit could not be fetched: " + why);
                return;
            }
            // Source: wolflifts_lib.php wlf_give -> {have: true} | {have: false, given: {name, item}, folder, folderId,
            // folderNew, delivered}
            const bool have = reply["have"].asBoolean();
            const LLUUID folder = reply["folderId"].asUUID();
            const LLUUID item_id = reply["given"].isMap() ? reply["given"]["item"].asUUID() : LLUUID::null;
            LL_INFOS("WolfLifts") << "kit give: " << (have ? "the grid says you already have one" : "given item " + item_id.asString())
                                  << " folder " << folder << (reply["folderNew"].asBoolean() ? " (new)" : "") << LL_ENDL;
            if (have) f->setStatus("You have a lift kit already - finding it in your inventory...", false);
            // the kit's folder may be new to this viewer (made by the grid just now, or while they were logged in): Objects is
            // asked for again so that it appears, then the folder itself and the kit folders (once each a round)
            bool objects_asked = false;
            for (S32 i = 0; i < GIVE_WAIT_S && !kit; ++i)
            {
                if (i % 5 == 4)
                {
                    asked.clear();   // the kit's and the kit folders, and unfinished copies, again every 5 s (Objects once)
                    search.again();
                }
                if (!objects_asked && (have || folder.isNull() || !gInventory.getCategory(folder)))
                {
                    objects_asked = refetchFolder(objectsFolder(), asked, "Objects");
                }
                if (folder.notNull()) refetchFolder(folder, asked, "the kit's");
                for (const LLUUID& k : kitFolders()) refetchFolder(k, asked, "kit");
                llcoro::suspendUntilTimeout(1.f);
                if (!self()) return;
                kit = item_id.notNull() ? gInventory.getItem(item_id) : nullptr;
                if (kit && !kit->isFinished()) kit = nullptr;
                if (!kit) kit = search.find();
            }
            f = self();
            if (!f) return;
            if (!kit) kit = findKit();   // one whole scan before giving up
            LL_INFOS("WolfLifts") << "kit give: " << (kit ? "found " + kit->getUUID().asString()
                                                          : llformat("not seen after %d s", GIVE_WAIT_S)) << LL_ENDL;
            if (!kit)
            {
                // a kit put down by hand is found by the hello of step 1 (lift_setup.lsl answers its owner)
                f->mBoxCoroRunning = false;
                f->boxFailed("Your lift kit is in your inventory (Objects > Cable Cars and Ski Lifts), but this viewer could not see it yet. "
                             "You can drag it from Objects > Cable Cars and Ski Lifts onto the ground beside you instead, or close the "
                             "window and open it again.");
                return;
            }
        }
    }
    // 3. rez it just in front of the avatar, on the ground; its "hi" is the one taken
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) { f->mBoxCoroRunning = false; f->boxFailed("You are not in a region just now."); return; }
    const LLUUID region = rgn->getRegionID();
    LLVector3 at = rgn->getPosRegionFromAgent(gAgent.getPositionAgent()) + LLVector3(gAgent.getAtAxis().mV[VX], gAgent.getAtAxis().mV[VY], 0.f) * 1.5f;
    at.mV[VZ] = rgn->getLand().resolveHeightRegion(at.mV[VX], at.mV[VY]) + 0.3f;
    std::set<LLUUID> before;
    for (S32 i = 0, n = gObjectList.getNumObjects(); i < n; ++i)
    {
        LLViewerObject* o = gObjectList.getObject(i);
        if (o && !o->isDead() && o->isRoot()) before.insert(o->getID());
    }
    f->setStatus("Putting your lift kit down beside you (it tidies itself away after building)...", false);
    LL_INFOS("WolfLifts") << "putting the lift kit down at " << at << LL_ENDL;   // [W104 FEATFIX 2026-11-11/] one line a kit
    rezAt(kit, at);
    for (S32 t = 0; t < 40; ++t)
    {
        llcoro::suspendUntilTimeout(0.5f);
        f = self();
        if (!f) return;
        if (!gAgent.getRegion() || gAgent.getRegion()->getRegionID() != region) { f->mBoxCoroRunning = false; f->boxFailed("You left the region while the lift kit was being put down."); return; }
        if (f->mWaitFor.isNull())
        {
            for (S32 i = 0, n = gObjectList.getNumObjects(); i < n; ++i)
            {
                LLViewerObject* o = gObjectList.getObject(i);
                if (!o || o->isDead() || !o->isRoot() || before.count(o->getID()) || o->getRegion() != gAgent.getRegion()) continue;
                if (dist_vec(o->getPositionRegion(), at) > 3.f) continue;
                f->mWaitFor = o->getID();
                break;
            }
        }
        if (f->mWaitFor.notNull() && t % 4 == 0) f->say("WLIFT|*|hello");
        if (f->mBox == BOX_READY) { f->mBoxCoroRunning = false; return; }
    }
    f->mBoxCoroRunning = false;
    if (f->mBox != BOX_READY)
    {
        f->boxFailed(f->mWaitFor.isNull()
            ? "The region did not put your lift kit down. Check that you may build here (the land's About Land > Options), then try again."
            : "Your lift kit was put down but did not answer. Check that scripts may run here, then close the window and try again.");
    }
}

// ---- the camera (wolffloatertunnels.cpp lookFromAbove / giveCameraBack) -----------------------------------------------------------

void WolfFloaterLifts::lookFromAbove()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn || LLApp::isExiting()) return;
    if (gAgentCamera.cameraMouselook()) gAgentCamera.changeCameraToDefault();
    if (!mCamSaved)
    {
        mCamSaved = true;
        mCamWasOnAvatar = gAgentCamera.getFocusOnAvatar();
        mCamPos = gAgentCamera.getCameraPositionGlobal();
        mCamFocus = gAgentCamera.getFocusTargetGlobal();
    }
    LLVector2 c;
    F32 span = 0.f;
    if (!mClicks.empty())
    {
        LLVector2 lo = mClicks[0], hi = mClicks[0];
        for (const LLVector2& p : mClicks)
        {
            lo.set(llmin(lo.mV[0], p.mV[0]), llmin(lo.mV[1], p.mV[1]));
            hi.set(llmax(hi.mV[0], p.mV[0]), llmax(hi.mV[1], p.mV[1]));
        }
        c = (lo + hi) * 0.5f;
        span = llmax(hi.mV[0] - lo.mV[0], hi.mV[1] - lo.mV[1]);
    }
    else
    {
        const LLVector3 me = rgn->getPosRegionFromAgent(gAgent.getPositionAgent());
        c.set(me.mV[VX], me.mV[VY]);
    }
    const F32 g = rgn->getLand().resolveHeightRegion(c.mV[0], c.mV[1]);
    const F32 half = llmax(0.05f, tanf(LLViewerCamera::getInstance()->getView() * 0.5f));
    // A click finds the ground only within the draw distance (llviewerwindow.cpp mousePointOnLandGlobal walks the ray to
    // gAgentCamera.mDrawDistance; the default is 128 m): the camera stays low enough for the whole view to be clickable.
    // A longer line is laid out by moving the camera along it (Alt + drag, or the camera controls) between clicks.
    const F32 reach = llmax(40.f, gAgentCamera.mDrawDistance * 0.55f);
    const F32 h = llmin(reach, llmax(TOP_VIEW_M, 0.6f * span / half));
    const LLVector3d focus = rgn->getPosGlobalFromRegion(LLVector3(c.mV[0], c.mV[1], g));
    const LLVector3d cam = focus + LLVector3d(0.0, -(F64)(h * TOP_VIEW_TILT), (F64)h);
    gAgentCamera.setFocusOnAvatar(false, false);
    gAgentCamera.setCameraPosAndFocusGlobal(cam, focus, LLUUID::null);
    mCamFocusSet = focus;
}

void WolfFloaterLifts::giveCameraBack()
{
    if (!mCamSaved) return;
    mCamSaved = false;
    const bool still_ours = !gAgentCamera.getFocusOnAvatar() && (gAgentCamera.getFocusTargetGlobal() - mCamFocusSet).magVecSquared() < 0.01;
    if (!still_ours) return;
    if (mCamWasOnAvatar) gAgentCamera.setFocusOnAvatar(true, true, false);
    else gAgentCamera.setCameraPosAndFocusGlobal(mCamPos, mCamFocus, LLUUID::null);
}

// ---- the pen (wolffloaterterraform.cpp penAllowed / keepPen) ---------------------------------------------------------------------

bool WolfFloaterLifts::penAllowed() const
{
    if (!mPenWanted || mBuilding || !getVisible() || isMinimized() || !onWolf() || LLApp::isExiting()) return false;
    if (LLFloaterReg::instanceVisible("wolf_tunnels") || LLFloaterReg::instanceVisible("wolf_terraform")) return false;
    return gAgent.getRegion() != nullptr;
}

void WolfFloaterLifts::keepPen()
{
    if (!WolfToolTunnel::instanceExists() && !mPenWanted) return;
    WolfToolTunnel* t = WolfToolTunnel::getInstance();
    if (!penAllowed())
    {
        if (t->pen() != WolfToolTunnel::PEN_NONE && t->hostIs(this)) t->cancel();
        return;
    }
    if (t->current()) return;
    if (gFloaterTools && gFloaterTools->getVisible()) return;
    if (LLToolMgr::getInstance()->usingTransientTool() || gFocusMgr.getMouseCapture() || gAgentCamera.cameraMouselook()) return;
    t->begin(getHandle(), WolfToolTunnel::PEN_ROUTE);
}

// ---- the window ---------------------------------------------------------------------------------------------------------------

void WolfFloaterLifts::draw()
{
    if (mStartPending && !LLApp::isExiting())
    {
        // here and not in onOpen: the Build window's focus would put the pen down at once (wolffloaterterraform.cpp draw)
        mStartPending = false;
        if (gFloaterTools && gFloaterTools->getVisible())
        {
            gFloaterTools->closeFloater();
            setStatus("The Build window was closed so that your clicks lay out the lift.", false);
        }
        lookFromAbove();
        mPenWanted = true;
    }
    LLViewerRegion* rgn = gAgent.getRegion();
    if (rgn && rgn->getRegionID() != mRegionAt)
    {
        // [W104 FEATFIX 2026-11-11 A1] a region change mid-build too: the kit's lines (llRegionSayTo) do not reach another
        // region, so the window stops waiting rather than stay locked
        const bool was_building = mBuilding;
        if (mBuilding) endBuilding(std::string(), false);
        if (was_building) setStatus("You left the region while your lift was being built. The lift kit carries on by itself - go back to see the lift.", true);
        else if (!mClicks.empty() && mRegionAt.notNull()) setStatus("You are on another region now: the drawing was put away.", true);
        mRegionAt = rgn->getRegionID();
        mClicks.clear();
        mPlan.clear();
        mPlanOk = false;
        stopBox();   // a box belongs to the region it is in; a coroutine looking for one stops (A5)
        changed();
    }
    if (mPlanDirty && mBox == BOX_READY && LLFrameTimer::getElapsedSeconds() - mChangedAt >= PLAN_WAIT_S) sendPlan();
    if (mBuilding && LLFrameTimer::getElapsedSeconds() - mBuildNewsAt > BUILD_GIVE_UP_S)
    {
        // [W104 FEATFIX 2026-11-11 A1] the real timeout: the window can never stay disabled
        endBuilding("Nothing has been heard from your lift kit for 2 minutes, so this window has stopped waiting. Look in the "
                    "world: if the lift is not there, press Build again.", true);
        changed();   // the plan is asked for again, so Build can be pressed
    }
    else if (mBuilding && LLFrameTimer::getElapsedSeconds() - mBuildNewsAt > BUILD_QUIET_S && !mQuietSaid)
    {
        mQuietSaid = true;
        setStatus("The lift kit has not said anything for a minute. If you left it, come back near it; it gives up by itself "
                  "and takes away what it had built if the region stops answering.", true);
    }
    updateControls();
    keepPen();
    LLFloater::draw();
}

// [W104 FEATFIX 2026-11-11 A7] updateControls runs every frame (draw): a text box is only set when its words change, as
// wolffloaterhuds.cpp refreshTiles does - LLTextBox::setText lays the text out again each time.
void WolfFloaterLifts::setTextIfChanged(LLTextBox* box, const std::string& text)
{
    if (box && box->getText() != text) box->setText(text);
}

void WolfFloaterLifts::updateControls()
{
    if (!mHint) return;
    setTextIfChanged(mHint, onWolf() ? routeHint() : std::string("Cable Cars and Ski Lifts works on Wolf Territories regions only."));
    mKind->setEnabled(!mBuilding);
    mUndoBtn->setEnabled(!mBuilding && !mClicks.empty());
    mStartOverBtn->setEnabled(!mBuilding && !mClicks.empty());
    mBuildBtn->setEnabled(!mBuilding && mPlanOk && !mPlanDirty && mBox == BOX_READY && mClicks.size() >= 2);
    std::string info;
    if (mClicks.size() >= 2 && !mPlanDirty && mBox == BOX_READY) info = mPlanText;
    setTextIfChanged(mPlanInfo, info);
}
