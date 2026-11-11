/**
 * @file wolfgreedysetup.cpp
 * @brief WolfViewer: "Put a Greedy table here". Design: wolfgreedysetup.h.
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

#include "wolfgreedysetup.h"

#include <cctype>
#include <cmath>
#include <set>

#include "fscommon.h"
#include "llagent.h"
#include "llbutton.h"
#include "llcoros.h"
#include "lleventcoro.h"
#include "llfloaterreg.h"
#include "lliconctrl.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llradiogroup.h"
#include "llsurface.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "lltooldraganddrop.h"
#include "llviewerinventory.h"
#include "llviewerobjectlist.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "message.h"
#include "wolfcave.h"
#include "wolfgrid.h"
#include "wolfobjectprops.h"

namespace
{
    // Source: region-terrain-work/greedy-dice-2026-11-10/server/wolfdice_give.php - gives one piece to a Wolf resident who has
    // none (deployed beside wolfdice.php on both web hosts; the lift kit's give uses the same host: wolffloaterlifts.cpp GIVE_URL)
    const char* GIVE_URL = "https://wolfstorm.app/php/wolfdice_give.php";
    constexpr F32 GIVE_TIMEOUT_S = 30.f;           // wolffloaterlifts.cpp GIVE_TIMEOUT_S
    constexpr S32 GIVE_WAIT_S = 30;                // the given item is looked for this many seconds (wolffloaterlifts.cpp boxCoro)
    constexpr S32 APPEAR_TRIES = 40;               // x 0.5 s: the rezzed piece is looked for 20 s (wolfvehiclerez.cpp APPEAR_SECS)
    constexpr S32 SETTLE_TRIES = 4;                // x 0.5 s: a match waits this long for other new things of ours not named yet
                                                   // (wolfvehiclerez.cpp SETTLE_SECS 2.0)
    constexpr F32 FOUND_XY_M = 2.5f;               // a new root of ours this near the spot (across the ground) is the piece
    constexpr F32 SAME_SPOT_M = 3.f;               // a piece this window put down this near the new spot: "already one here"
    constexpr F32 EDGE_M = 2.f;                    // the spot keeps this far inside the region (a table is about 2 x 1.2 m)
    // Source: llselectmgr.h UPD_POSITION 0x01, UPD_ROTATION 0x02, UPD_LINKED_SETS 0x08; OpenSim LLClientView.cs
    // HandleMultipleObjUpdate case 0x0B "(8 + 2 + 1) group rotation and position": the whole linkset, position first
    constexpr U8 UPD_GROUP_POS_ROT = 0x0B;

    // The pieces. Source: server/wolfdice_items.php (name, asset, the description's first word), build_wolfdice.py
    // TABLE_DESC / FOLDER_NAME, build_jimmy_table.py NAME / DESC; how far in front: lsl/wolfdice_setup.lsl TABLE_AHEAD / BOARD_AHEAD.
    // rootZ, the root's height over the floor, as the set up box puts it (wolfdice_setup.lsl pos.z = feet + TABLE_ROOT_Z):
    // build_wolfdice.py table root_z 0.78 and board root_z 1.575; build_jimmy_table.py the Sandbox copy's gz = ground + 0.49 + 0.02.
    // The region's own placing is not used for the height: it raised the Wolf Grid table about 30 m (its root lies on its side,
    // and OpenSim's rez height comes from the object's box; seen 2026-11-11 on Sandbox 2).
    struct Piece { const char* key; const char* name; const char* asset; const char* prefix; F32 ahead; F32 rootZ; const char* what; };
    const Piece PIECES[] = {
        { "table",          "Greedy Table",             "5ae30a6b-a81f-4998-b64a-86ebeca689da", "greedy-table", 2.6f, 0.78f,  "table" },
        { "table_wolfgrid", "Greedy Table (Wolf Grid)", "1df63892-4ab9-4e5d-83cd-55d9727fa13b", "greedy-table", 2.6f, 0.51f,  "table" },
        { "board",          "Greedy Board",             "a92c4c97-9cbc-4785-bdd8-5b942f0b5a5f", "greedy board", 2.0f, 1.575f, "board" },
    };
    constexpr S32 PIECE_BOARD = 2;

    // [W104 review 2026-11-11 W2] pieces sent to the region and not yet placed, by any coroutine. A rez once sent is placed even
    // when the window has been closed (else the Wolf Grid table stays where the region's rez height put it, ~30 m up); while one
    // is out, Put waits, so two rezzes can never be looking for their piece at the same time.
    S32 sPlacing = 0;
    struct PlacingGuard
    {
        PlacingGuard() { ++sPlacing; }
        ~PlacingGuard() { done(); }
        void done() { if (mOn) { --sPlacing; mOn = false; } }
        bool mOn = true;
        PlacingGuard(const PlacingGuard&) = delete;
        PlacingGuard& operator=(const PlacingGuard&) = delete;
    };
    const char* WAIT_FOR_OLD = "The piece you asked for before is still being put down. Please wait a moment.";

    /** wolfdice_give_lib.php wdg_desc_is: the description starts with the word (leading spaces and case aside), and it ends there. */
    bool descIs(std::string d, const std::string& word)
    {
        LLStringUtil::trimHead(d);
        LLStringUtil::toLower(d);
        if (d.compare(0, word.size(), word) != 0) return false;
        if (d.size() == word.size()) return true;
        const char next = d[word.size()];
        return !(isalnum((unsigned char)next) || next == '_' || next == '-');
    }

    // The resident's copy of a piece: the server's "have it" (wolfdice_give_lib.php wdg_have) - an object item (not a link)
    // outside the Trash whose asset is the piece's, or whose name is the piece's (case and outer spaces aside) and whose
    // description starts with its word (both tables start "greedy-table", so the name tells them apart).
    // Source: wolffloaterlifts.cpp KitItems (wolfvehiclerez.cpp VehicleItems, the same test).
    class PieceItems : public LLInventoryCollectFunctor
    {
    public:
        explicit PieceItems(const Piece& p) : mPiece(p), mAsset(p.asset) {}
        bool operator()(LLInventoryCategory*, LLInventoryItem* item) override
        {
            if (!item || item->getIsLinkType() || item->getType() != LLAssetType::AT_OBJECT
                || item->getInventoryType() != LLInventoryType::IT_OBJECT) return false;
            if (item->getAssetUUID() == mAsset) return true;
            std::string n = item->getName();
            LLStringUtil::trim(n);
            return LLStringUtil::compareInsensitive(n, std::string(mPiece.name)) == 0 && descIs(item->getDescription(), mPiece.prefix);
        }
    private:
        const Piece& mPiece;
        const LLUUID mAsset;
    };

    LLViewerInventoryItem* findPiece(const Piece& p)
    {
        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t items;
        PieceItems match(p);
        gInventory.collectDescendentsIf(gInventory.getRootFolderID(), cats, items, LLInventoryModel::EXCLUDE_TRASH, match);
        for (const LLPointer<LLViewerInventoryItem>& it : items)
        {
            if (it.notNull() && it->isFinished()) return it.get();
        }
        return nullptr;
    }

    /** Rez `item` with its root over `at` (region metres), the bottom of its box on at.z. Source: wolffloaterlifts.cpp rezAt
     *  (wolffloatertunnels.cpp rezTunnel; lltooldraganddrop.cpp:1970-2028 dropObject). RayEndIsIntersection = 1 places it at
     *  RayEnd (OpenSim Scene.cs GetNewRezLocation), and a single object is raised by its root's height over the bottom of its
     *  box (InventoryAccessModule.cs RezObject: pos.Z += offsetHeight); RayStart = RayEnd keeps the water test from moving it. */
    void rezAt(LLViewerInventoryItem* item, const LLVector3& at, const LLUUID& group)
    {
        LLViewerRegion* rgn = gAgent.getRegion();
        if (!rgn || !item) return;
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_RezObject);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->addUUIDFast(_PREHASH_GroupID, group);   // FSCommon::getGroupForRezzing(), taken once by the caller
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

    /** Move the whole linkset of root `o` to `pos` (region metres) and turn it to `rot`. Source: fsfloaterimport.cpp
     *  setPrimPosition (MultipleObjectUpdate, one block: position, then the rotation packed with packToVector3 as
     *  llselectmgr.cpp packMultipleUpdate). */
    void placeAt(LLViewerObject* o, const LLVector3& pos, const LLQuaternion& rot)
    {
        if (!o || !o->getRegion()) return;
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_MultipleObjectUpdate);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_ObjectLocalID, o->getLocalID());
        msg->addU8Fast(_PREHASH_Type, UPD_GROUP_POS_ROT);
        U8 data[24];
        htolememcpy(data, pos.mV, MVT_LLVector3, 12);
        LLVector3 vec = rot.packToVector3();
        htolememcpy(data + 12, vec.mV, MVT_LLQuaternion, 12);
        msg->addBinaryDataFast(_PREHASH_Data, data, 24);
        msg->sendReliable(o->getRegion()->getHost());
    }

    F32 distXY(const LLVector3& a, const LLVector3& b)
    {
        return LLVector2(a.mV[VX] - b.mV[VX], a.mV[VY] - b.mV[VY]).length();
    }
}

WolfFloaterGreedySetup::WolfFloaterGreedySetup(const LLSD& key) : LLFloater(key) {}

bool WolfFloaterGreedySetup::postBuild()
{
    mWhich = getChild<LLRadioGroup>("gs_which");
    mMessage = getChild<LLTextEditor>("gs_message");
    mWhich->setSelectedIndex(0);
    getChild<LLButton>("gs_put")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPut(); });
    getChild<LLButton>("gs_back")->setCommitCallback([this](LLUICtrl*, const LLSD&) { setMessage("", false); setStep(mStep - 1); });
    // the middle button: Skip on the table step, Finish on the board step, Start again at the end (lsl/wolfdice_setup.lsl:
    // [Rez table] [Skip] [Close], [Rez board] [Back] [Finish], [Start again] [Close])
    getChild<LLButton>("gs_skip")->setCommitCallback([this](LLUICtrl*, const LLSD&) { setMessage("", false); setStep(mStep == 3 ? 1 : mStep + 1); });
    getChild<LLButton>("gs_close")->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
    setStep(1);
    return true;
}

void WolfFloaterGreedySetup::onOpen(const LLSD& key)
{
    // [W104 review 2026-11-11 B1] LLFloater::openFloater calls onOpen on every showInstance, also when the window is already
    // open (llfloater.cpp openFloater: onOpen after the "already open" branch) - the Greedy window's TABLE HERE and the HUD's
    // press it again. A piece on its way keeps its window as it is: resetting mBusy here re-enabled Put while the first
    // coroutine was still out, and a second table was rezzed.
    if (mBusy) return;
    // a fresh start each time it is opened (single_instance floaters are kept, llfloater.cpp mReuseInstance); a coroutine of
    // an earlier opening stops touching this window (its generation moved on)
    ++mGen;
    const bool wolf = WolfGrid::isOnWolfTerritories();
    setMessage(!wolf ? std::string("Greedy tables are put down here on Wolf Territories. On other grids, rez the table from your "
                                   "inventory (Objects > Greedy), if you have one.")
               : sPlacing > 0 ? std::string(WAIT_FOR_OLD) : std::string(), !wolf);
    setStep(1);
}

void WolfFloaterGreedySetup::onClose(bool app_quitting)
{
    // a piece still being fetched stops here; one already sent to the region is still placed, and what became of it is said in
    // nearby chat (putCoro, [W104 review W2])
    ++mGen;
    mBusy = false;
}

void WolfFloaterGreedySetup::setMessage(const std::string& text, bool error)
{
    // Source: wolffloatergreedy.cpp showMessage - light red on the dark window, and the words say it too; it stays until the
    // next press (never taken away by a timer)
    const LLUIColor c = error ? LLUIColor(LLColor4(1.f, 0.6f, 0.6f, 1.f)) : LLUIColor(LLColor4::white);
    mMessage->setReadOnlyColor(c);
    mMessage->setColor(c);
    mMessage->setText(text);
    mMessage->setVisible(!text.empty());
}

void WolfFloaterGreedySetup::setStep(S32 step)
{
    mStep = llclamp(step, 1, 3);
    // The words of lsl/wolfdice_setup.lsl steps(), for a window
    LLTextBox* title = getChild<LLTextBox>("gs_title");
    LLTextBox* label = getChild<LLTextBox>("gs_step");
    LLTextBox* body = getChild<LLTextBox>("gs_body");
    if (mStep == 1)
    {
        title->setText(std::string("Let's set up Greedy!"));
        label->setText(std::string("Step 1 of 2: the table"));
        body->setText(std::string("Stand where you want the table, facing the way you want to look at it. Then press the big "
                                  "button. The table appears just in front of you, facing you."));
        getChild<LLButton>("gs_put")->setLabel(std::string("Put the table here"));
        getChild<LLButton>("gs_skip")->setLabel(std::string("Skip"));
    }
    else if (mStep == 2)
    {
        title->setText(std::string("Let's set up Greedy!"));
        label->setText(std::string("Step 2 of 2: the board"));
        body->setText(std::string("The board shows the top 10 players and the games being played now. Stand where you want it, "
                                  "facing the wall or spot, then press the big button. Or press Finish if you don't want one."));
        getChild<LLButton>("gs_put")->setLabel(std::string("Put the board here"));
        getChild<LLButton>("gs_skip")->setLabel(std::string("Finish"));
    }
    else
    {
        title->setText(std::string("Greedy is set up!"));
        label->setText(std::string("Have fun!"));
        body->setText(std::string("Sit at the table to play - your friends can sit down too. The Greedy window (Wolf World > "
                                  "Greedy) also plays from anywhere."));
        getChild<LLButton>("gs_skip")->setLabel(std::string("Start again"));
    }
    refreshControls();
}

void WolfFloaterGreedySetup::refreshControls()
{
    const bool wolf = WolfGrid::isOnWolfTerritories();
    getChild<LLUICtrl>("gs_which_label")->setVisible(mStep == 1);
    mWhich->setVisible(mStep == 1);
    mWhich->setEnabled(!mBusy);
    for (const char* n : { "gs_die_a", "gs_die_b", "gs_die_c" }) getChild<LLUICtrl>(n)->setVisible(mStep == 3);
    getChild<LLUICtrl>("gs_put")->setVisible(mStep < 3);
    getChild<LLUICtrl>("gs_put")->setEnabled(wolf && !mBusy && sPlacing == 0);   // [W104 review W2] one rez out at a time
    getChild<LLUICtrl>("gs_skip")->setEnabled(!mBusy);
    getChild<LLUICtrl>("gs_back")->setVisible(mStep == 2);
    getChild<LLUICtrl>("gs_back")->setEnabled(!mBusy);
}

void WolfFloaterGreedySetup::onPut()
{
    if (mBusy || mStep >= 3) return;
    if (sPlacing > 0)
    {
        setMessage(WAIT_FOR_OLD, false);
        return;
    }
    if (!WolfGrid::isOnWolfTerritories())
    {
        setMessage("Greedy tables are put down here on Wolf Territories only.", true);
        return;
    }
    const S32 piece = mStep == 2 ? PIECE_BOARD : llclamp(mWhich->getSelectedIndex(), 0, 1);
    mBusy = true;
    refreshControls();
    LLHandle<LLFloater> h = getHandle();
    const U32 gen = mGen;
    LLCoros::instance().launch("WolfGreedySetup", [h, gen, piece]() { putCoro(h, gen, piece); });
}

// static
void WolfFloaterGreedySetup::putCoro(LLHandle<LLFloater> h, U32 gen, S32 index)
{
    // The window is looked up again through its handle after every wait (wolffloaterlifts.cpp boxCoro): closed or opened
    // afresh meanwhile (its generation moved on), it is no longer this press's window.
    auto self = [&h, gen]() -> WolfFloaterGreedySetup*
    {
        WolfFloaterGreedySetup* f = dynamic_cast<WolfFloaterGreedySetup*>(h.get());
        return f && f->mGen == gen ? f : nullptr;
    };
    // the window whatever its generation (single_instance: kept when closed) - what it has put down is remembered there
    auto any = [&h]() -> WolfFloaterGreedySetup* { return dynamic_cast<WolfFloaterGreedySetup*>(h.get()); };
    const Piece& p = PIECES[index];
    const std::string name(p.name);
    const std::string what(p.what);
    // before the rez is sent: a closed window stops it, and the words go to the window
    auto fail = [&self](const std::string& why)
    {
        if (WolfFloaterGreedySetup* f = self())
        {
            f->mBusy = false;
            f->setMessage(why, true);
            f->refreshControls();
        }
    };

    // 1. where it goes: in front of them, facing them (lsl/wolfdice_setup.lsl rezPiece). [W104 review 2026-11-11 W1] worked
    // out again after the give (step 2 can wait a minute): from where they stand and look then, not where they were.
    LLVector3 at, fwd;
    auto where = [&](LLViewerRegion* rgn, WolfFloaterGreedySetup* f, std::string& why) -> bool
    {
        if (!rgn || !isAgentAvatarValid()) { why = "You are not in a region just now."; return false; }
        if (gAgentAvatarp->isSitting()) { why = "Please stand up first, then stand where you want the " + what + " and press the button again."; return false; }
        if (gAgent.getFlying()) { why = "Please land first (stop flying), then stand where you want the " + what + " and press the button again."; return false; }
        const LLVector3 me = rgn->getPosRegionFromAgent(gAgent.getPositionAgent());
        fwd.set(gAgent.getAtAxis().mV[VX], gAgent.getAtAxis().mV[VY], 0.f);
        if (fwd.length() < 0.01f) fwd.set(1.f, 0.f, 0.f);
        fwd.normalize();
        at = me + fwd * p.ahead;
        const F32 width = rgn->getWidth();
        if (at.mV[VX] < EDGE_M || at.mV[VY] < EDGE_M || at.mV[VX] > width - EDGE_M || at.mV[VY] > width - EDGE_M)
        {
            why = "You are too near the edge of the region for the " + what + ". Walk a few steps inwards, then press the button again.";
            return false;
        }
        // the surface they stand on: their feet (llGetAgentSize / 2 below the avatar's centre, as the box), never under the ground
        const F32 ground = rgn->getLand().resolveHeightRegion(at.mV[VX], at.mV[VY]);
        at.mV[VZ] = llmax(ground, me.mV[VZ] - gAgentAvatarp->mBodySize.mV[VZ] * 0.5f);
        for (const auto& placed : f->mPlaced)
        {
            LLViewerObject* o = gObjectList.findObject(placed.first);
            if (o && !o->isDead() && o->getRegion() == rgn && distXY(o->getPositionRegion(), at) < SAME_SPOT_M)
            {
                // the piece that is in the way, by name (a board pressed right in front of the new table is "the table")
                why = "Your Greedy " + placed.second + " is right where the " + what + " would go. Walk a few steps away, or turn "
                      "round, then press the button again.";
                return false;
            }
        }
        return true;
    };

    WolfFloaterGreedySetup* f = self();
    if (!f) return;
    LLViewerRegion* rgn = gAgent.getRegion();
    std::string why;
    if (!where(rgn, f, why)) { fail(why); return; }
    // [W104 review W1] the region they pressed in: a move to another while the piece is fetched is said, not rezzed over there
    const LLUUID region = rgn->getRegionID();

    // 2. their copy, or the grid gives one (wolffloaterlifts.cpp boxCoro step 2)
    LLViewerInventoryItem* item = findPiece(p);
    if (!item)
    {
        f->setMessage("Getting your " + name + " from the grid (it goes into your inventory: Objects > Greedy)...", false);
        const std::string body = std::string("{\"action\":\"give\",\"piece\":\"") + p.key + "\"}";
        LLSD result = WolfCaves::serviceRequest(GIVE_URL, &body, GIVE_TIMEOUT_S);
        LLSD reply;
        std::string transport;
        const S32 status = WolfCaves::serviceReply(result, reply, transport);
        if (!(f = self())) return;
        if (status != 200 || !reply.isMap() || !reply["success"].asBoolean())
        {
            // a 404: the give is not on this web host (not installed yet, or being updated) - said in words, not "Not Found"
            const std::string err = reply.isMap() && reply.has("error") ? reply["error"].asString()
                : status == 404 ? std::string("the grid does not hand out Greedy tables just now. Please try again later, or ask Wolf Territories support.")
                : (transport.empty() ? llformat("the grid answered HTTP %d", status) : transport);
            fail("Your " + name + " could not be fetched: " + err);
            return;
        }
        const LLUUID folder = reply["folderId"].asUUID();
        const LLUUID item_id = reply["given"].isMap() ? reply["given"]["item"].asUUID() : LLUUID::null;
        for (S32 i = 0; i < GIVE_WAIT_S && !item; ++i)
        {
            if (folder.notNull())
            {
                if (LLViewerInventoryCategory* cat = gInventory.getCategory(folder)) cat->setVersion(LLViewerInventoryCategory::VERSION_UNKNOWN);
                gInventory.fetchDescendentsOf(folder);
            }
            llcoro::suspendUntilTimeout(1.f);
            if (!self()) return;
            item = item_id.notNull() ? gInventory.getItem(item_id) : nullptr;
            if (item && !item->isFinished()) item = nullptr;
            if (!item) item = findPiece(p);
        }
        if (!(f = self())) return;
        if (!item)
        {
            fail("Your " + name + " is in your inventory (Objects > Greedy), but this viewer could not see it yet. Wait a moment, "
                 "then press the button again.");
            return;
        }
        // [W104 review W1] the same region, and the spot from where they are now
        rgn = gAgent.getRegion();
        if (!rgn || rgn->getRegionID() != region)
        {
            fail("You are in another region now, so your " + name + " was not put down. It is in your inventory (Objects > Greedy). "
                 "Stand where you want it, then press the button again.");
            return;
        }
        if (!where(rgn, f, why)) { fail(why); return; }
    }

    // 3. rez it; the new root of ours, named as the item, that appears over the spot is it
    std::set<LLUUID> before;
    for (S32 i = 0, n = gObjectList.getNumObjects(); i < n; ++i)
    {
        LLViewerObject* o = gObjectList.getObject(i);
        if (o && !o->isDead() && o->isRoot()) before.insert(o->getID());
    }
    // OpenSim InventoryAccessModule.cs DoPreRezWhenFromItem: a single object's root takes the item's Name and Description
    const std::string item_name = item->getName();
    const std::string item_desc = item->getDescription();
    const LLUUID group = FSCommon::getGroupForRezzing();
    // where it is moved to and how it is turned, fixed now: the placing below no longer looks at the window
    // 4 (below): turned to face them - the piece keeps its own upright turn (the Wolf Grid table lies on its side,
    // build_jimmy_table.py; its script's NATURAL_ROT), and the turn about the vertical goes on top of it, as
    // lsl/wolfdice_table.lsl llSetRot(NATURAL_ROT * rot) after the box's yaw-only rez (rot = llEuler2Rot(<0, 0, atan2(fwd.y, fwd.x)>));
    // and put on the floor at the box's height (rootZ above it), over the spot
    const LLQuaternion yaw(atan2f(fwd.mV[VY], fwd.mV[VX]), LLVector3::z_axis);
    const LLVector3 dest(at.mV[VX], at.mV[VY], at.mV[VZ] + p.rootZ);
    f->setMessage("Putting your " + name + " down in front of you...", false);
    LL_INFOS("WolfGreedy") << "putting " << name << " down at " << at << " in " << rgn->getName() << LL_ENDL;
    rezAt(item, at, group);

    // [W104 review 2026-11-11 W2] from here the piece is on its way and is placed whatever happens to the window. While the
    // press's window is still open the words go there; closed (or opened afresh), they go to nearby chat.
    PlacingGuard placing;
    auto tell = [&](const std::string& text, bool error, S32 next_step)
    {
        placing.done();
        if (WolfFloaterGreedySetup* w = self())
        {
            w->mBusy = false;
            if (next_step > 0) w->setStep(next_step);
            w->setMessage(text, error);
            w->refreshControls();
            return;
        }
        FSCommon::report_to_nearby_chat("Greedy: " + text);
        if (WolfFloaterGreedySetup* w = any())
        {
            // opened again meanwhile: Put waited for this one (WAIT_FOR_OLD); it may go now
            if (w->mMessage && w->mMessage->getText() == WAIT_FOR_OLD) w->setMessage("", false);
            w->refreshControls();
        }
    };

    WolfObjectProps& props = WolfObjectProps::instance();
    LLViewerObject* got = nullptr;
    S32 settle = 0;
    for (S32 t = 0; t < APPEAR_TRIES && !got; ++t)
    {
        llcoro::suspendUntilTimeout(0.5f);
        LLViewerRegion* now_rgn = gAgent.getRegion();
        if (!now_rgn || now_rgn->getRegionID() != region)
        {
            tell("You left the region while the " + what + " was being put down. If it appeared, it is still there, where you "
                 "were standing.", true, 0);
            return;
        }
        // [W104 review W3] a candidate is new to the viewer, a root, not an avatar or anything worn, the agent's (or, rezzed
        // with a group, group-owned: llviewerobject.cpp permGroupOwner, FLAGS_OBJECT_GROUP_OWNED), over the spot, and named
        // and described as the item. OpenSim keeps the rezzer as owner when it sets the rez group (InventoryAccessModule.cs
        // RezObject: group.SetGroup(groupID, ...), the owner stays remoteClient.AgentId), so permYouOwner is what normally
        // matches; group ownership is accepted for a grid that deeds on rez. Names arrive only in ObjectPropertiesFamily, asked
        // for through the shared store (wolfvehiclerez.cpp tickRezzing). The nearest match wins.
        LLViewerObject* best = nullptr;
        F32 best_d = 0.f;
        bool unnamed = false;
        for (S32 i = 0, n = gObjectList.getNumObjects(); i < n; ++i)
        {
            LLViewerObject* o = gObjectList.getObject(i);
            if (!o || o->isDead() || !o->isRoot() || o->isAvatar() || o->isAttachment() || o->isHUDAttachment()
                || before.count(o->getID()) || o->getRegion() != now_rgn) continue;
            if (!(o->permYouOwner() || (group.notNull() && o->permGroupOwner()))) continue;
            const F32 d = distXY(o->getPositionRegion(), at);
            if (d > FOUND_XY_M) continue;
            const WolfObjectProps::Props* pr = props.get(o->getID());
            if (!pr)
            {
                props.want(o);
                unnamed = true;
                continue;
            }
            if (pr->mName != item_name || !(pr->mDescription == item_desc || descIs(pr->mDescription, p.prefix)))
            {
                before.insert(o->getID());   // another of the agent's things: never looked at again
                continue;
            }
            if (!best || d < best_d) { best = o; best_d = d; }
        }
        if (!best) continue;
        // another new thing of ours over the spot not named yet: a moment for it, it may be the nearer one
        if (unnamed && settle < SETTLE_TRIES && t + 1 < APPEAR_TRIES) { ++settle; continue; }
        got = best;
    }
    if (!got)
    {
        // Source: wolfvehiclerez.cpp tickRezzing - the sim refuses a rez without saying so when the parcel is over its limits
        // (InventoryAccessModule.cs RezObject); only a refusal of its own checks says "Can't rez object ...".
        // [W104 review W3] the build right is only known for the agent's own parcel (llviewerparcelmgr.cpp allowAgentBuild:
        // mAgentParcel; the viewer holds no other parcel's flags), so it is used only when the spot is on that parcel
        // (inAgentParcel, the parcel overlay). The piece may only be late: they are told to look and wait, not to press again.
        LLViewerParcelMgr* pm = LLViewerParcelMgr::getInstance();
        const LLViewerRegion* now_rgn = gAgent.getRegion();
        const bool own_parcel = now_rgn && pm->inAgentParcel(now_rgn->getPosGlobalFromRegion(at));
        if (own_parcel && !pm->allowAgentBuild())
        {
            tell("This land does not let you build, so the " + what + " could not be put here. Go to land where you may build "
                 "(a sandbox, or your own land), then press the button again.", true, 0);
        }
        else
        {
            tell("The " + what + " has not appeared yet. It may just be slow: please look around you and wait a minute before you "
                 "press the button again, so you do not get two. If it still does not come, the land may be full (its object "
                 "limit)" + std::string(own_parcel ? "" : " or may not let you build there") + ". A sandbox or your own land is "
                 "the easiest place to try.", true, 0);
        }
        return;
    }

    // 4. turned to face them and put at the box's height (dest and yaw above)
    placeAt(got, dest, got->getRotationRegion() * yaw);
    if (WolfFloaterGreedySetup* w = any()) w->mPlaced.emplace_back(got->getID(), what);
    LL_INFOS("WolfGreedy") << name << " is down: " << got->getID() << LL_ENDL;
    if (index == PIECE_BOARD)
    {
        tell(self() ? std::string("Your Greedy Board is up. It shows the top 10 and the games being played now.")
                    : std::string("Your Greedy Board is up, just in front of where you were standing."), false, 3);
    }
    else
    {
        tell(self() ? "Your " + name + " is here! Sit at it to play. Next, the board - or press Finish."
                    : "Your " + name + " is down, just in front of where you were standing. Sit at it to play.", false, 2);
    }
}
