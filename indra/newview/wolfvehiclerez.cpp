/**
 * @file wolfvehiclerez.cpp
 * @brief WolfViewer: the HUDs' REZ button. See wolfvehiclerez.h.
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

#include "wolfvehiclerez.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <vector>

#include "fscommon.h"
#include "llagent.h"
#include "llcallbacklist.h"
#include "llframetimer.h"
#include "llhudeffecttrail.h"
#include "llhudmanager.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llinventoryfunctions.h"
#include "llnotificationsutil.h"
#include "llselectmgr.h"
#include "lltooldraganddrop.h"
#include "llui.h"
#include "llviewerinventory.h"
#include "llviewermenu.h"
#include "llviewerobjectlist.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llviewerstats.h"
#include "llvoavatarself.h"
#include "llworld.h"
#include "message.h"
#include "pipeline.h"
#include "rlvactions.h"
#include "rlvhandler.h"
#include "wolfflight.h"
#include "wolfgrid.h"
#include "wolfobjectprops.h"

// [HUD REZ 2026-10-12] How a REZ press runs, one stage after another, from an idle callback that is registered
// only while a press is being worked on (never polling between presses):
//   FINDING    the item: the agent's own inventory, then the Library, waiting for the inventory to load;
//   COPYING    a Library item into the agent's inventory first (OpenSim cannot rez a Library item, see startCopy);
//   APPEARING  the RezObject is sent: waiting for the new object to arrive in the object list;
//   SITTING    the AgentRequestSit is sent: waiting for the avatar to be seated on it.
namespace
{
    enum EStage { STAGE_IDLE = 0, STAGE_FINDING, STAGE_COPYING, STAGE_APPEARING, STAGE_SITTING };

    // How long each stage may take before the driver is told it did not work. Long enough for a busy sim and a
    // first inventory load, short enough that the lit button never means "stuck".
    const F64 FIND_SECS = 60.0;     // an inventory that is still loading after this is reported, not waited on
    const F64 COPY_SECS = 30.0;
    const F64 APPEAR_SECS = 30.0;
    const F64 SIT_SECS = 15.0;
    // How often each stage is looked at: the scans of the whole inventory (FINDING) and of the whole object list
    // (APPEARING) once a second; the rest (a copy's deadline, is the avatar seated) four times a second.
    const F64 SCAN_SECS = 1.0;
    const F64 TICK_SECS = 0.25;

    // The new object is the agent's own root that was not there before the rez, within this distance of the
    // rez point (the sim moves it onto the surface and lifts it by its bounding box, InventoryAccessModule.cs
    // RezObject GetNewRezLocation / offsetHeight).
    const F32 APPEAR_RADIUS = 20.f;
    // Two matching candidates whose distances from the rez point differ by less than this are too close to tell
    // apart: neither is sat on, the driver is told to sit on theirs.
    const F32 AMBIGUOUS_GAP = 4.f;
    // Once one candidate matches, this long for the others in range to be named before choosing between them.
    const F64 SETTLE_SECS = 2.0;

    const std::string FREE_FOLDER = "free wolf vehicles";   // compared lower-cased
    const std::string DESC_PREFIX = "wolfvehicle:";

    struct State
    {
        EStage      mStage = STAGE_IDLE;
        std::string mType;              // car, truck, ... (the description word)
        F64         mDeadline = 0.0;
        F64         mNextTick = 0.0;
        U32         mGeneration = 0;    // one per press: a late Library copy callback for an old press is ignored
        bool        mFetchStarted = false;
        LLUUID      mFetchAsked;        // an unfinished item asked for once (LLViewerInventoryItem::fetchFromServer)
        LLUUID      mItemId;            // the agent's item that was rezzed
        std::string mItemName;
        LLVector3d  mRezGlobal;         // where it was rezzed
        LLVector3   mHeading;           // the avatar's heading when REZ was pressed (flat, unit)
        U64         mRegionHandle = 0;  // the agent's region when it was rezzed
        std::set<LLUUID> mBefore;       // every root in the object list when the RezObject was sent: never the new one
        std::map<LLUUID, F64> mFirstSeen;   // roots not in mBefore, when this press first saw them (after the send)
        F64         mSentAt = 0.0;      // when the RezObject was sent
        F64         mMatchedAt = 0.0;   // when the first matching candidate was seen (0: none yet)
        bool        mNoCopy = false;    // a no-copy item: taken out of the inventory list when it was sent
        LLUUID      mNewId;             // the object it became
        bool        mIdle = false;
    };

    State& st()
    {
        static State s;
        return s;
    }

    void idle(void*);

    void alert(const std::string& msg)
    {
        LLNotificationsUtil::add("GenericAlertOK", LLSD().with("MESSAGE", msg));
    }

    // The vehicle's name in a sentence.
    std::string niceName(const std::string& type)
    {
        if (type == "bike") return "motorbike";
        if (type == "plane") return "aeroplane";
        if (type == "sailboat") return "sailing boat";
        return type;
    }

    // How far in front of the avatar the vehicle's middle goes, metres: clear of the avatar for its size, and
    // further for the aircraft so they have room.
    F32 aheadFor(const std::string& type)
    {
        if (type == "bike") return 3.f;
        if (type == "car") return 4.f;
        if (type == "speedboat") return 6.f;
        if (type == "truck" || type == "sailboat") return 7.f;
        if (type == "submarine") return 8.f;
        if (type == "helicopter") return 9.f;
        if (type == "spaceship") return 10.f;
        if (type == "plane") return 12.f;
        return 5.f;
    }

    void finish(const std::string& msg)
    {
        State& s = st();
        s.mStage = STAGE_IDLE;
        s.mBefore.clear();
        s.mFirstSeen.clear();
        if (s.mIdle)
        {
            gIdleCallbacks.deleteFunction(idle, nullptr);
            s.mIdle = false;
        }
        if (!msg.empty())
        {
            alert(msg);
        }
    }

    void stage(EStage next, F64 secs)
    {
        State& s = st();
        s.mStage = next;
        s.mDeadline = LLFrameTimer::getElapsedSeconds() + secs;
        if (!s.mIdle)
        {
            gIdleCallbacks.addFunction(idle, nullptr);
            s.mIdle = true;
        }
    }

    bool rlvBlocksRez()
    {
        // Source: lltooldraganddrop.cpp:1901-1906 dropObject() / :2488-2493 dad3dRezObjectOnLand() - no rez from
        // inventory while RLV @rez or @interact is on
        return RlvActions::isRlvEnabled() && (gRlvHandler.hasBehaviour(RLV_BHVR_REZ) || gRlvHandler.hasBehaviour(RLV_BHVR_INTERACT));
    }

    //-------------------------------------------------------------------------
    // finding the item
    //-------------------------------------------------------------------------

    // "wolfvehicle:<type>" at the start of the description (case and leading spaces aside), and the word ends
    // there: "wolfvehicle:car" is a car, "wolfvehicle:cart" is not.
    bool descIs(const std::string& desc, const std::string& type)
    {
        std::string d = desc;
        LLStringUtil::trimHead(d);
        LLStringUtil::toLower(d);
        const std::string want = DESC_PREFIX + type;
        if (d.compare(0, want.size(), want) != 0)
        {
            return false;
        }
        if (d.size() == want.size())
        {
            return true;
        }
        const char next = d[want.size()];
        return !isalnum((unsigned char)next) && next != '_';
    }

    bool descIsAnyVehicle(const std::string& desc)
    {
        std::string d = desc;
        LLStringUtil::trimHead(d);
        LLStringUtil::toLower(d);
        return d.compare(0, DESC_PREFIX.size(), DESC_PREFIX) == 0;
    }

    // An object item (not a link: the item it points at is found itself) for `type`, or for any vehicle when
    // `type` is empty.
    class VehicleItems : public LLInventoryCollectFunctor
    {
    public:
        explicit VehicleItems(const std::string& type) : mType(type) {}
        bool operator()(LLInventoryCategory*, LLInventoryItem* item) override
        {
            if (!item || item->getIsLinkType()
                || item->getType() != LLAssetType::AT_OBJECT || item->getInventoryType() != LLInventoryType::IT_OBJECT)
            {
                return false;
            }
            return mType.empty() ? descIsAnyVehicle(item->getDescription()) : descIs(item->getDescription(), mType);
        }
    private:
        std::string mType;
    };

    bool inFreeFolder(const LLInventoryItem* item)
    {
        LLUUID id = item->getParentUUID();
        for (S32 depth = 0; id.notNull() && depth < 64; ++depth)
        {
            const LLViewerInventoryCategory* cat = gInventory.getCategory(id);
            if (!cat)
            {
                return false;
            }
            std::string name = cat->getName();
            LLStringUtil::trim(name);
            LLStringUtil::toLower(name);
            if (name == FREE_FOLDER)
            {
                return true;
            }
            id = cat->getParentUUID();
        }
        return false;
    }

    // The item to rez under `root` (the agent's inventory, Trash left out, or the Library), a "Free Wolf Vehicles"
    // folder's first. Worn ones are left alone (lltooldraganddrop.cpp:2507-2511 dad3dRezObjectOnLand(): a worn
    // attachment is never rezzed).
    LLViewerInventoryItem* findUnder(const LLUUID& root, const std::string& type)
    {
        if (root.isNull())
        {
            return nullptr;
        }
        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t items;
        VehicleItems match(type);
        gInventory.collectDescendentsIf(root, cats, items, LLInventoryModel::EXCLUDE_TRASH, match);
        LLViewerInventoryItem* any = nullptr;
        for (const LLPointer<LLViewerInventoryItem>& p : items)
        {
            LLViewerInventoryItem* item = p.get();
            if (!item || (isAgentAvatarValid() && gAgentAvatarp->isWearingAttachment(item->getUUID())))
            {
                continue;
            }
            if (inFreeFolder(item))
            {
                return item;
            }
            if (!any)
            {
                any = item;
            }
        }
        return any;
    }

    bool libraryHasVehicles()
    {
        const LLUUID& lib = gInventory.getLibraryRootFolderID();
        if (lib.isNull())
        {
            return false;
        }
        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t items;
        VehicleItems match("");
        gInventory.collectDescendentsIf(lib, cats, items, LLInventoryModel::EXCLUDE_TRASH, match);
        return !items.empty();
    }

    std::string notFoundMessage(const std::string& type)
    {
        const std::string n = niceName(type);
        std::string msg = "You don't have a free Wolf " + n + " in your inventory yet.\n\n"
                          "Take a free copy on the Sandbox region: it goes into your inventory, and REZ finds it there.";
        if (libraryHasVehicles())
        {
            msg += "\n\nThe Library has free Wolf vehicles too, but no " + n + " yet.";
        }
        msg += "\n\n(REZ looks for an object whose description starts \"" + DESC_PREFIX + type + "\".)";
        return msg;
    }

    bool seatedOn(const LLUUID& root_id)
    {
        if (!isAgentAvatarValid() || !gAgentAvatarp->isSitting() || !gAgentAvatarp->getParent())
        {
            return false;
        }
        const LLViewerObject* seat = (LLViewerObject*)gAgentAvatarp->getParent();
        const LLViewerObject* root = seat->getRootEdit();
        return root && root->getID() == root_id;
    }

    //-------------------------------------------------------------------------
    // rezzing
    //-------------------------------------------------------------------------

    // Where it goes: `ahead` metres in front of the avatar along its heading, on what is under that point - a
    // prim (a floor, a deck, a platform) or the land, and on the water's surface where that is higher (the sim
    // does that: Scene.cs GetNewRezLocation, wpos). Like a drag from inventory: on a prim it is
    // dad3dRezObjectOnObject's ray, on the land dad3dRezObjectOnLand's (lltooldraganddrop.cpp:2548 / :2638).
    struct Spot
    {
        LLViewerRegion* mRegion = nullptr;
        LLVector3d mStart;      // the ray's start (in a drag, the camera: lltooldraganddrop.cpp:1958 mLastCameraPos)
        LLVector3d mHit;        // what it lands on (in a drag, mLastHitPos)
        LLUUID mTarget;         // the prim it lands on; null for the land
        bool mOnLand = true;
    };

    bool findSpot(const std::string& type, const LLVector3& heading, Spot& out)
    {
        const LLVector3d agent = gAgent.getPositionGlobal();
        const LLVector3d p = agent + LLVector3d(heading * aheadFor(type));
        LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(p);
        if (!region)
        {
            return false;
        }
        const F32 ground = LLWorld::getInstance()->resolveLandHeightGlobal(p);
        const F32 water = region->getWaterHeight();
        // from above the avatar's head (or the water, so a boat's spot over water always crosses the surface)
        // straight down to just under the land
        const F32 top = llmax((F32)agent.mdV[VZ] + 1.f, water + 1.f);
        const F32 bottom = llmin(ground - 1.f, top - 2.f);
        const LLVector3d start(p.mdV[VX], p.mdV[VY], top);
        const LLVector3d end(p.mdV[VX], p.mdV[VY], bottom);

        LLVector4a s4, e4, hit4;
        s4.load3(gAgent.getPosAgentFromGlobal(start).mV);
        e4.load3(gAgent.getPosAgentFromGlobal(end).mV);
        S32 face = -1;
        LLViewerObject* hit = gPipeline.lineSegmentIntersectInWorld(s4, e4, true, false, true, false, &face, nullptr, nullptr, &hit4);
        const bool on_prim = hit && !hit->isDead() && hit->getPCode() == LL_PCODE_VOLUME && !hit->isAvatar()
                          && !hit->isAttachment() && !hit->isHUDAttachment();
        out.mRegion = region;
        out.mStart = start;
        if (on_prim)
        {
            out.mHit = gAgent.getPosGlobalFromAgent(LLVector3(hit4.getF32ptr()));
            out.mTarget = hit->getID();
            out.mOnLand = false;
            LLViewerRegion* hit_region = LLWorld::getInstance()->getRegionFromPosGlobal(out.mHit);
            if (hit_region)
            {
                out.mRegion = hit_region;
            }
        }
        else
        {
            out.mHit = LLVector3d(p.mdV[VX], p.mdV[VY], ground);
            out.mTarget.setNull();
            out.mOnLand = true;
        }
        return true;
    }

    // Source: lltooldraganddrop.cpp:1887-2062 dropObject() - the RezObject a drag from inventory sends, field for
    // field (message_template.msg RezObject), with the drag's camera and hit point replaced by the Spot.
    void sendRezObject(LLViewerInventoryItem* item, const Spot& spot)
    {
        LLViewerRegion* regionp = spot.mRegion;

        // Source: lltooldraganddrop.cpp:1909
        make_ui_sound("UISndObjectRezIn");

        // Source: lltooldraganddrop.cpp:1923-1928 - a no-copy item goes into the world (the sim takes it out of
        // inventory: InventoryAccessModule.cs DoPostRezWhenFromItem)
        bool remove_from_inventory = false;
        if (!item->getPermissions().allowCopyBy(gAgent.getID()))
        {
            remove_from_inventory = true;
        }
        st().mNoCopy = remove_from_inventory;

        // Source: lltooldraganddrop.cpp:2548 / :2638 - on the land dropObject(obj, true, ...), on a prim
        // dropObject(obj, false, ...): bypass_sim_raycast
        const bool bypass_sim_raycast = spot.mOnLand;
        // Source: lltooldraganddrop.cpp:1932-1942 - the ray is limited to the one prim hit (none for the land)
        const LLUUID ray_target_id = spot.mTarget;

        // Source: lltooldraganddrop.cpp:1955 - a drag rezzes selected only while the edit tool is out
        // (LLToolMgr::inEdit()). Never here: a REZ is pressed on a HUD, not in the edit tool, and a selected object
        // is held still by the sim (InventoryAccessModule.cs RezObject: RezSelected -> group.IsSelected) - the
        // driver is about to sit in it.
        const bool rez_selected = false;

        // Source: lltooldraganddrop.cpp:1958-1967 - region coordinates; for a prim the ray is doubled past it
        LLVector3 ray_start = regionp->getPosRegionFromGlobal(spot.mStart);
        LLVector3 ray_end = regionp->getPosRegionFromGlobal(spot.mHit);
        if (!bypass_sim_raycast)
        {
            LLVector3 ray_direction = ray_start - ray_end;
            ray_end = ray_end - ray_direction;
        }

        // Source: lltooldraganddrop.cpp:1970-2028
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_RezObject);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->addUUIDFast(_PREHASH_GroupID, FSCommon::getGroupForRezzing());

        msg->nextBlock("RezData");
        msg->addUUIDFast(_PREHASH_FromTaskID, LLUUID::null);   // from the agent's inventory (dropObject: source_id)
        msg->addU8Fast(_PREHASH_BypassRaycast, (U8)bypass_sim_raycast);
        msg->addVector3Fast(_PREHASH_RayStart, ray_start);
        msg->addVector3Fast(_PREHASH_RayEnd, ray_end);
        msg->addUUIDFast(_PREHASH_RayTargetID, ray_target_id);
        msg->addBOOLFast(_PREHASH_RayEndIsIntersection, false);
        msg->addBOOLFast(_PREHASH_RezSelected, rez_selected);
        msg->addBOOLFast(_PREHASH_RemoveItem, remove_from_inventory);

        // Source: lltooldraganddrop.cpp:2003, :3575-3586 pack_permissions_slam()
        pack_permissions_slam(msg, item->getFlags(), item->getPermissions());

        // Source: lltooldraganddrop.cpp:2005-2026 - the item itself (never a Library or Trash one here, so the
        // FolderID is left as it is)
        msg->nextBlockFast(_PREHASH_InventoryData);
        item->packMessage(msg);
        msg->sendReliable(regionp->getHost());

        // Source: lltooldraganddrop.cpp:2030-2032 <FS:Techwolf Lupindo> - no default build parameters for it
        FSCommon::sObjectAddMsg = 0;

        // Source: lltooldraganddrop.cpp:2045-2052
        if (remove_from_inventory)
        {
            gInventory.deleteObject(item->getUUID());
            gInventory.notifyObservers();
        }

        // Source: lltooldraganddrop.cpp:2054-2061 - VEFFECT: DropObject, and the rez counted
        LLHUDEffectSpiral* effectp = (LLHUDEffectSpiral*)LLHUDManager::getInstance()->createViewerEffect(LLHUDObject::LL_HUD_EFFECT_BEAM, true);
        effectp->setSourceObject(gAgentAvatarp);
        effectp->setPositionGlobal(spot.mHit);
        effectp->setDuration(LL_HUD_DUR_SHORT);
        effectp->setColor(LLColor4U(gAgent.getEffectColor()));
        add(LLStatViewer::OBJECT_REZ, 1);
    }

    // The item is the agent's own and complete: rez it, wait for it. (Paul 10-12: "dont delete the vehicle then let
    // the user do it" - REZ never stands the driver up and never takes anything back; an old vehicle is theirs to
    // delete.)
    void startRez(LLViewerInventoryItem* item)
    {
        State& s = st();
        if (rlvBlocksRez())
        {
            finish("Your " + niceName(s.mType) + " can't be rezzed now: an RLV restriction (rez) is on.");
            return;
        }
        if (!gAgent.getRegion() || !isAgentAvatarValid())
        {
            finish("Your " + niceName(s.mType) + " can't be rezzed now: you are not in a region yet.");
            return;
        }
        // [WOLF GRID GATE 2026-10-17] rez() checked the grid when the button was pressed; the RezObject goes from here,
        // after the inventory search, so a hypergrid teleport in between would have rezzed it on the foreign region.
        if (!WolfGrid::isOnWolfTerritoriesConfirmed())
        {
            finish("Your free Wolf " + niceName(s.mType) + " is for Wolf Territories regions, so it was not rezzed here.");
            return;
        }
        Spot spot;
        if (!findSpot(s.mType, s.mHeading, spot))
        {
            finish("There is no room in front of you for your " + niceName(s.mType) + " (that is the edge of the world). "
                   "Turn round and press REZ again.");
            return;
        }

        s.mItemId = item->getUUID();
        s.mItemName = item->getName();
        s.mRezGlobal = spot.mHit;
        s.mRegionHandle = gAgent.getRegion()->getHandle();
        s.mNewId.setNull();
        s.mMatchedAt = 0.0;
        // every root the viewer has now, wherever it is and whoever owns it: none of them is the new one. The
        // viewer has no creation time for an object short of selecting it (ObjectProperties CreationDate, which
        // freezes it sim-side), so "new" is "not here when the RezObject went": mBefore, then mFirstSeen.
        s.mBefore.clear();
        s.mFirstSeen.clear();
        for (S32 i = 0, n = gObjectList.getNumObjects(); i < n; ++i)
        {
            LLViewerObject* o = gObjectList.getObject(i);
            if (o && !o->isDead() && o->isRoot())
            {
                s.mBefore.insert(o->getID());
            }
        }

        sendRezObject(item, spot);
        s.mSentAt = LLFrameTimer::getElapsedSeconds();
        LL_INFOS("WolfVehicleRez") << "rezzed '" << s.mItemName << "' (" << s.mType << ") at " << s.mRezGlobal
                                   << (spot.mOnLand ? " on the land" : " on a prim") << LL_ENDL;
        stage(STAGE_APPEARING, APPEAR_SECS);
    }

    // OpenSim cannot rez a Library item: RezObject looks the item up in the agent's own inventory only
    // (InventoryAccessModule.cs:903-913 RezObject -> InventoryService.GetItem; XInventoryService.cs:611-619 reads
    // the agent's inventory table, and the Library lives in the LibraryService, not there) and silently does
    // nothing. So a Library vehicle is first copied into the agent's inventory, exactly as the viewer does for a
    // Library object dragged onto the avatar - Source: lltooldraganddrop.cpp:2457-2471 dad3dRezAttachmentFromInv()
    // SOURCE_LIBRARY: copy_inventory_item(agent, the item's owner, the item, LLUUID::null, "", cb) - and the copy is
    // rezzed. The null folder puts it in the agent's Objects folder (Scene.Inventory.cs:125-147 AddInventoryItem:
    // GetFolderForType); the next REZ finds it there and copies nothing.
    void startCopy(LLViewerInventoryItem* lib_item)
    {
        State& s = st();
        const U32 gen = s.mGeneration;
        LLPointer<LLInventoryCallback> cb = new LLBoostFuncInventoryCallback([gen](const LLUUID& new_id)
        {
            State& s = st();
            if (s.mGeneration != gen || s.mStage != STAGE_COPYING)
            {
                return;   // a newer press, or this one already ended
            }
            LLViewerInventoryItem* item = gInventory.getItem(new_id);
            if (item && item->isFinished())
            {
                startRez(item);
            }
            else
            {
                stage(STAGE_FINDING, FIND_SECS);   // it is in the agent's inventory now: found there
            }
        });
        // Source: llviewerinventory.cpp:1353-1373 copy_inventory_item()
        copy_inventory_item(gAgent.getID(), lib_item->getPermissions().getOwner(), lib_item->getUUID(), LLUUID::null, std::string(), cb);
        stage(STAGE_COPYING, COPY_SECS);
    }

    void tickFinding()
    {
        State& s = st();
        if (gInventory.isInventoryUsable())
        {
            if (LLViewerInventoryItem* item = findUnder(gInventory.getRootFolderID(), s.mType))
            {
                // Source: lltooldraganddrop.cpp:1913 - only a complete item is rezzed; an incomplete one is asked for
                // (LLViewerInventoryItem::fetchFromServer) and waited on
                if (item->isFinished())
                {
                    startRez(item);
                    return;
                }
                if (s.mFetchAsked != item->getUUID())
                {
                    s.mFetchAsked = item->getUUID();
                    item->fetchFromServer();
                }
            }
            else
            {
                LLInventoryModelBackgroundFetch& fetch = LLInventoryModelBackgroundFetch::instance();
                const bool all = fetch.isEverythingFetched();
                if (all || fetch.inventoryFetchCompleted())
                {
                    const LLUUID& lib = gInventory.getLibraryRootFolderID();
                    if (LLViewerInventoryItem* lib_item = findUnder(lib, s.mType))
                    {
                        startCopy(lib_item);
                        return;
                    }
                    if (all || lib.isNull() || fetch.libraryFetchCompleted())
                    {
                        finish(notFoundMessage(s.mType));
                        return;
                    }
                }
            }
        }
        if (LLFrameTimer::getElapsedSeconds() > s.mDeadline)
        {
            finish("Your inventory is still loading, so your free Wolf " + niceName(s.mType) + " couldn't be found yet. "
                   "Wait a moment and press REZ again.");
        }
    }

    // Source: llselectmgr.cpp:4894-4950 sendMultipleUpdate() / packMultipleUpdate() - the edit tool's rotation:
    // UPD_ROTATION | UPD_LINKED_SETS (llselectmgr.h:61-63), the rotation packed to 12 bytes (packToVector3). The
    // sim turns the whole linkset (LLClientView.cs HandleMultipleObjUpdate case 0x0A: groupR), which the owner
    // may do even without modify rights (SceneGraph.cs:1494-1532 ClientChangeObject: CanMoveObject).
    void sendRotation(LLViewerObject* obj, const LLQuaternion& rot)
    {
        LLViewerRegion* region = obj->getRegion();
        if (!region)
        {
            return;
        }
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_MultipleObjectUpdate);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_ObjectLocalID, obj->getLocalID());
        msg->addU8Fast(_PREHASH_Type, UPD_ROTATION | UPD_LINKED_SETS);
        U8 data[12];
        const LLVector3 vec = rot.packToVector3();
        htolememcpy(data, vec.mV, MVT_LLQuaternion, 12);
        msg->addBinaryDataFast(_PREHASH_Data, data, 12);
        msg->sendReliable(region->getHost());
    }

    // Facing the avatar's heading: the vehicle's forward axis (local +X, the LSL vehicle's default reference frame)
    // turned about the vertical onto the heading REZ was pressed with. Only the turn: its tilt as made is kept.
    void faceHeading(LLViewerObject* obj, const LLVector3& heading)
    {
        const LLQuaternion rot = obj->getRotation();
        const LLVector3 fwd = LLVector3::x_axis * rot;
        if (fwd.mV[VX] * fwd.mV[VX] + fwd.mV[VY] * fwd.mV[VY] < 0.01f)
        {
            return;   // made pointing straight up or down: no heading to turn
        }
        const F32 have = atan2f(fwd.mV[VY], fwd.mV[VX]);
        const F32 want = atan2f(heading.mV[VY], heading.mV[VX]);
        LLQuaternion turned = rot * LLQuaternion(want - have, LLVector3::z_axis);
        turned.normalize();
        sendRotation(obj, turned);
    }

    void tickAppearing()
    {
        State& s = st();
        if (!gAgent.getRegion() || gAgent.getRegion()->getHandle() != s.mRegionHandle)
        {
            finish("You left the region before your " + niceName(s.mType) + " appeared. If it was rezzed it is where you were; "
                   "it stays there until you take it back.");
            return;
        }
        const F64 now = LLFrameTimer::getElapsedSeconds();
        const LLVector3 rez_agent = gAgent.getPosAgentFromGlobal(s.mRezGlobal);
        WolfObjectProps& props = WolfObjectProps::instance();
        struct Cand { LLViewerObject* o; F32 dist; };
        std::vector<Cand> matches;
        bool unnamed = false;
        for (S32 i = 0, n = gObjectList.getNumObjects(); i < n; ++i)
        {
            LLViewerObject* o = gObjectList.getObject(i);
            if (!o || o->isDead() || !o->isRoot() || s.mBefore.count(o->getID()))
            {
                continue;
            }
            // first seen after the send (it was not in mBefore): noted once, whatever it is, so it is only ever
            // weighed as new from the moment it arrived
            s.mFirstSeen.emplace(o->getID(), now);
            if (o->isAvatar() || o->isAttachment() || !o->permYouOwner())
            {
                continue;
            }
            const F32 dist = dist_vec(o->getPositionAgent(), rez_agent);
            if (dist > APPEAR_RADIUS)
            {
                continue;
            }
            // New, the agent's, where it was rezzed: it is a candidate when its name and description are the item's
            // (InventoryAccessModule.cs DoPreRezWhenFromItem: a single object's root takes the item's Name and
            // Description). Names arrive only in ObjectPropertiesFamily: asked for through the shared store.
            const WolfObjectProps::Props* p = props.get(o->getID());
            if (!p)
            {
                props.want(o);
                unnamed = true;
                continue;
            }
            if (p->mName != s.mItemName || !descIs(p->mDescription, s.mType))
            {
                s.mBefore.insert(o->getID());   // another of the agent's things: never looked at again
                continue;
            }
            matches.push_back({ o, dist });
        }
        if (!matches.empty())
        {
            if (s.mMatchedAt == 0.0)
            {
                s.mMatchedAt = now;
            }
            // another new thing of the agent's in range not named yet: give it a moment, it may be a second copy
            if (unnamed && now - s.mMatchedAt < SETTLE_SECS && now <= s.mDeadline)
            {
                return;
            }
            std::sort(matches.begin(), matches.end(), [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
            if (matches.size() > 1 && matches[1].dist - matches[0].dist < AMBIGUOUS_GAP)
            {
                // Two of the same vehicle, both new to the viewer, as near the spot as each other: one may be an
                // older copy that only now came into view. Neither is chosen.
                LL_INFOS("WolfVehicleRez") << matches.size() << " matching '" << s.mItemName << "' near the rez point: not sitting" << LL_ENDL;
                finish("Your " + niceName(s.mType) + " is here, but there is more than one " + niceName(s.mType)
                       + " of yours just in front of you, so you weren't sat in one. Right-click yours and choose Sit Here.");
                return;
            }
            LLViewerObject* o = matches[0].o;
            s.mNewId = o->getID();
            LL_INFOS("WolfVehicleRez") << "'" << s.mItemName << "' appeared: " << s.mNewId << " "
                                       << matches[0].dist << " m from the rez point, "
                                       << (s.mFirstSeen[s.mNewId] - s.mSentAt) << " s after the send" << LL_ENDL;
            // Source: llviewermenu.cpp:5944-5976 handle_object_sit() - the Sit Here of the pie menu, which refuses
            // silently under RLV (RlvActions::canSit); said here instead, and before it is turned, so a refused sit
            // leaves the vehicle exactly as the sim put it
            if (RlvActions::isRlvEnabled() && !RlvActions::canSit(o))
            {
                finish("Your " + niceName(s.mType) + " is here, but you can't sit on it now: an RLV restriction is on.");
                return;
            }
            // Source: llviewermenu.cpp:5955-5961 - under RLV @standtp a Sit while seated only stands the avatar up
            if (RlvActions::isRlvEnabled() && gRlvHandler.hasBehaviour(RLV_BHVR_STANDTP)
                && isAgentAvatarValid() && gAgentAvatarp->isSitting())
            {
                finish("Your " + niceName(s.mType) + " is here, but you are sitting on something else and an RLV restriction "
                       "(standtp) means sitting elsewhere only stands you up. Stand up, then right-click it and choose Sit Here.");
                return;
            }
            faceHeading(o, s.mHeading);
            handle_object_sit(o->getID());   // llviewermenu.cpp:5996-6006: on the object, offset 0 (its sit target)
            stage(STAGE_SITTING, SIT_SECS);
            return;
        }
        if (LLFrameTimer::getElapsedSeconds() > s.mDeadline)
        {
            std::string msg = "Your " + niceName(s.mType) + " didn't appear within " + llformat("%d", (S32)APPEAR_SECS) + " seconds.";
            // The sim refuses a rez on land that does not let the agent build without saying so when it is over the
            // parcel's limits (InventoryAccessModule.cs RezObject: CanRezObject -> return null); only its second
            // check sends "Can't rez object ..." (DoPreRezWhenFromItem).
            if (!LLViewerParcelMgr::getInstance()->allowAgentBuild())
            {
                msg += "\n\nThis parcel doesn't let you build or rez objects. Go to land that does (a sandbox, or your own) and press REZ again.";
            }
            else
            {
                msg += "\n\nThe parcel may be full (its object limit) or not let you rez here, or the region is busy. Try again in a moment, or somewhere else.";
            }
            if (s.mNoCopy)
            {
                // Source: lltooldraganddrop.cpp:2045-2052 - a no-copy item leaves the inventory list when it is sent;
                // a sim that refuses it sends it back (InventoryAccessModule.cs:1017-1027 SendBulkUpdateInventory)
                msg += "\n\nIt is a no-copy item, so it left your inventory when it was sent. If it was refused it comes back "
                       "into your inventory; if it doesn't, it reappears after you relog.";
            }
            finish(msg);
        }
    }

    void tickSitting()
    {
        State& s = st();
        if (seatedOn(s.mNewId))
        {
            finish("");
            return;
        }
        if (LLFrameTimer::getElapsedSeconds() > s.mDeadline)
        {
            finish("Your " + niceName(s.mType) + " is here, but sitting in it didn't work. Right-click it and choose Sit Here.");
        }
    }

    void idle(void*)
    {
        State& s = st();
        const F64 now = LLFrameTimer::getElapsedSeconds();
        if (now < s.mNextTick)
        {
            return;
        }
        s.mNextTick = now + ((s.mStage == STAGE_FINDING || s.mStage == STAGE_APPEARING) ? SCAN_SECS : TICK_SECS);
        switch (s.mStage)
        {
        case STAGE_FINDING:   tickFinding(); break;
        case STAGE_COPYING:
            if (now > s.mDeadline)
            {
                finish("Your free Wolf " + niceName(s.mType) + " couldn't be copied from the Library. Press REZ again, or drag it "
                       "from the Library into your inventory yourself.");
            }
            break;
        case STAGE_APPEARING: tickAppearing(); break;
        case STAGE_SITTING:   tickSitting(); break;
        case STAGE_IDLE:
        default:              finish(""); break;
        }
    }
}

namespace WolfVehicleRez
{
    std::string typeForDesign(WolfDrive::EType design)
    {
        switch (design)
        {
        case WolfDrive::TYPE_TRUCK:     return "truck";
        case WolfDrive::TYPE_BIKE:      return "bike";
        case WolfDrive::TYPE_SPEEDBOAT: return "speedboat";
        case WolfDrive::TYPE_SPACESHIP: return "spaceship";
        case WolfDrive::TYPE_SUBMARINE: return "submarine";
        case WolfDrive::TYPE_CAR:
        default:                        return "car";
        }
    }

    std::string typeForFlightDeck()
    {
        if (WolfFlight::instance().sailing())
        {
            return "sailboat";
        }
        // Source: wolfflight.cpp:3170-3173 WolfFlight::craft() - the CDU CONTROLS page's AEROPLANE / HELICOPTER
        return WolfFlight::craft() == WolfFlight::CRAFT_HELI ? "helicopter" : "plane";
    }

    void rez(const std::string& type)
    {
        State& s = st();
        if (!WolfGrid::isOnWolfTerritories())
        {
            return;   // the HUDs are Wolf-only; nothing off Wolf
        }
        if (s.mStage != STAGE_IDLE)
        {
            return;   // one at a time: the lit button says it is under way
        }
        if (rlvBlocksRez())
        {
            alert("Your " + niceName(type) + " can't be rezzed now: an RLV restriction (rez) is on.");
            return;
        }
        if (!gAgent.getRegion() || !isAgentAvatarValid())
        {
            return;
        }
        ++s.mGeneration;
        s.mType = type;
        s.mFetchAsked.setNull();
        LLVector3 at = gAgent.getAtAxis();
        at.mV[VZ] = 0.f;
        if (at.normVec() < 0.01f)
        {
            at = LLVector3::x_axis;
        }
        s.mHeading = at;
        // the inventory and the Library loaded in the background as the inventory window does (start(): the
        // agent's root and the Library root, recursively), unless that has already been done
        if (!LLInventoryModelBackgroundFetch::instance().isEverythingFetched())
        {
            LLInventoryModelBackgroundFetch::instance().start();
        }
        stage(STAGE_FINDING, FIND_SECS);
        s.mNextTick = 0.0;
        tickFinding();   // found at once when the inventory is loaded
    }

    bool pending()
    {
        return st().mStage != STAGE_IDLE;
    }

    std::string tip(const std::string& type)
    {
        const State& s = st();
        if (s.mStage != STAGE_IDLE)
        {
            switch (s.mStage)
            {
            case STAGE_FINDING:   return "Finding your free Wolf " + niceName(s.mType) + " in your inventory...";
            case STAGE_COPYING:   return "Copying your free Wolf " + niceName(s.mType) + " from the Library...";
            case STAGE_APPEARING: return "Rezzing your " + niceName(s.mType) + "...";
            case STAGE_SITTING:   return "Sitting you in your " + niceName(s.mType) + "...";
            default:              break;
            }
        }
        return "Rez your free Wolf " + niceName(type) + " in front of you and sit in it.";
    }
}
