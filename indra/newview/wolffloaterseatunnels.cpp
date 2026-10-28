/**
 * @file wolffloaterseatunnels.cpp
 * @brief WolfViewer: Sea Tunnels - glass tubes with air inside, under the sea. See wolffloaterseatunnels.h.
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

#include "wolffloaterseatunnels.h"

#include <set>

#include "fscommon.h"
#include "llagent.h"
#include "llbutton.h"
#include "llcoros.h"
#include "lleventcoro.h"
#include "llselectmgr.h"
#include "llspinctrl.h"
#include "lltextbox.h"
#include "lluicolortable.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "llvolume.h"
#include "llvolumemessage.h"
#include "material_codes.h"
#include "message.h"
#include "wolfdryvolumes.h"
#include "wolfgrid.h"
#include "wolfobjectprops.h"

namespace
{
    // The glass wall of a made piece, metres (thin enough to read as glass, thick enough for the region's physics to
    // keep a walker on it).
    constexpr F32 WALL_M = 0.3f;
    // LLVolumeParams::setHollow range (llvolume.h:613 "range 0 to 1"); the build floater offers up to 95 %.
    constexpr F32 HOLLOW_MAX = 0.95f;
    // OpenSim keeps at most 127 bytes of a description: SceneObjectPart.cs:1070 Description setter, new osUTF8(value, 127).
    constexpr size_t DESC_MAX = 127;
    const char* PIECE_NAME = "Sea tunnel piece";

    /** Source: wolfcavelights.cpp sendObjectAdd (from lltoolplacer.cpp LLToolPlacer::addObject): RayEndIsIntersection = 1
     *  puts the prim at RayEnd + 0.25 m (OpenSim Scene.cs GetNewRezLocation: pos.Z += 0.5 / 2 for the 0.5 m scale AddNewPrim
     *  passes), so RayEnd is the wanted centre 0.25 m lower. Not created selected. */
    void sendTube(const LLVector3& centre, const LLQuaternion& rot, const LLVector3& scale, F32 hollow, LLViewerRegion* rgn)
    {
        LLVolumeParams vp;
        vp.setType(LL_PCODE_PROFILE_CIRCLE, LL_PCODE_PATH_LINE);
        vp.setBeginAndEndS(0.f, 1.f);
        vp.setBeginAndEndT(0.f, 1.f);
        vp.setRatio(1.f, 1.f);
        vp.setShear(0.f, 0.f);
        vp.setHollow(hollow);
        const LLVector3 ray_end = centre - LLVector3(0.f, 0.f, 0.25f);
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_ObjectAdd);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->addUUIDFast(_PREHASH_GroupID, FSCommon::getGroupForRezzing());
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU8Fast(_PREHASH_Material, LL_MCODE_GLASS);
        msg->addU32Fast(_PREHASH_AddFlags, (U32)0);
        LLVolumeMessage::packVolumeParams(&vp, msg);
        msg->addU8Fast(_PREHASH_PCode, LL_PCODE_VOLUME);
        msg->addVector3Fast(_PREHASH_Scale, scale);
        msg->addQuatFast(_PREHASH_Rotation, rot);
        msg->addVector3Fast(_PREHASH_RayStart, ray_end);
        msg->addVector3Fast(_PREHASH_RayEnd, ray_end);
        msg->addU8Fast(_PREHASH_BypassRaycast, (U8)1);
        msg->addU8Fast(_PREHASH_RayEndIsIntersection, (U8)1);
        msg->addU8Fast(_PREHASH_State, (U8)0);
        msg->addUUIDFast(_PREHASH_RayTargetID, LLUUID::null);
        msg->sendReliable(rgn->getHost());
    }

    /** Source: wolfcavelights.cpp nameObject (from fsfloaterimport.cpp processPrimCreated): ObjectName / ObjectDescription. */
    void sendName(LLViewerObject* o, const std::string& name)
    {
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_ObjectName);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_LocalID, o->getLocalID());
        msg->addStringFast(_PREHASH_Name, name);
        msg->sendReliable(o->getRegion()->getHost());
    }

    void sendDescription(LLViewerObject* o, const std::string& desc)
    {
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_ObjectDescription);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_LocalID, o->getLocalID());
        msg->addStringFast(_PREHASH_Description, desc);
        msg->sendReliable(o->getRegion()->getHost());
    }

    /** The description with the keyword added at the front (kept within DESC_MAX), or every copy of it taken out. */
    std::string withAir(const std::string& desc, bool air)
    {
        std::string lower = desc;
        LLStringUtil::toLower(lower);
        const bool has = lower.find(WolfDryVolumes::KEYWORD) != std::string::npos;
        if (has == air) return desc;   // already as asked: nothing is sent, the description is left exactly as it is
        std::string out;
        size_t from = 0;
        for (size_t at = lower.find(WolfDryVolumes::KEYWORD); at != std::string::npos;
             at = lower.find(WolfDryVolumes::KEYWORD, from))
        {
            out += desc.substr(from, at - from);
            from = at + WolfDryVolumes::KEYWORD.size();
        }
        out += desc.substr(from);
        // tidy the spaces the word leaves
        std::string tidy;
        for (char c : out)
        {
            if (c == ' ' && (tidy.empty() || tidy.back() == ' ')) continue;
            tidy += c;
        }
        while (!tidy.empty() && tidy.back() == ' ') tidy.pop_back();
        if (!air) return tidy;
        std::string tagged = tidy.empty() ? WolfDryVolumes::KEYWORD : WolfDryVolumes::KEYWORD + " " + tidy;
        if (tagged.size() > DESC_MAX) tagged = utf8str_truncate(tagged, (S32)DESC_MAX);   // never half a character
        return tagged;
    }
}

WolfFloaterSeaTunnels::WolfFloaterSeaTunnels(const LLSD& key)
    : LLFloater(key)
{
}

WolfFloaterSeaTunnels::~WolfFloaterSeaTunnels() = default;

bool WolfFloaterSeaTunnels::postBuild()
{
    mLength = getChild<LLSpinCtrl>("length");
    mAcross = getChild<LLSpinCtrl>("across");
    mMakeBtn = getChild<LLButton>("make_btn");
    mFillBtn = getChild<LLButton>("fill_btn");
    mPlainBtn = getChild<LLButton>("plain_btn");
    mCount = getChild<LLTextBox>("count_text");
    mStatus = getChild<LLTextBox>("status_text");
    mMakeBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onMakePiece(); });
    mFillBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSetAir(true); });
    mPlainBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSetAir(false); });
    setStatus(std::string(), false);
    return true;
}

void WolfFloaterSeaTunnels::onOpen(const LLSD& key)
{
    // [WOLF GRID GATE 2026-10-17] (wolffloateragecheck.cpp): a Wolf-only window opened off Wolf says so and closes.
    if (!WolfGrid::isOnWolfTerritories())
    {
        WolfGrid::noteWolfOnly(getString("feature_name"));
        closeFloater();
        return;
    }
    updateControls();
    mRefresh.reset();
}

void WolfFloaterSeaTunnels::draw()
{
    // the count and the buttons follow the selection and the sweep (1.5 s); twice a second is plenty
    if (mRefresh.getElapsedTimeF32() > 0.5f)
    {
        updateControls();
        mRefresh.reset();
    }
    LLFloater::draw();
}

void WolfFloaterSeaTunnels::setStatus(const std::string& text, bool error)
{
    if (!mStatus) return;
    mStatus->setText(text);
    mStatus->setColor(LLUIColorTable::instance().getColor(error ? "AlertCautionTextColor" : "LabelTextColor"));
}

void WolfFloaterSeaTunnels::updateControls()
{
    const bool wolf = WolfGrid::isOnWolfTerritories();
    mMakeBtn->setEnabled(wolf && !mMaking && gAgent.getRegion() && isAgentAvatarValid());
    const bool any_selected = LLSelectMgr::getInstance()->getSelection()->getObjectCount() > 0;
    mFillBtn->setEnabled(wolf && any_selected);
    mPlainBtn->setEnabled(wolf && any_selected);
    LLStringUtil::format_map_t args;
    args["[N]"] = llformat("%d", WolfDryVolumes::instance().taggedCount());
    mCount->setText(getString("count_line", args));
}

void WolfFloaterSeaTunnels::onMakePiece()
{
    if (mMaking || !gAgent.getRegion() || !isAgentAvatarValid()) return;
    mMaking = true;
    updateControls();
    setStatus(getString("making"), false);
    const F32 length = (F32)mLength->getValue().asReal();
    const F32 across = (F32)mAcross->getValue().asReal();
    LLHandle<LLFloater> h = getHandle();
    LLCoros::instance().launch("WolfSeaTunnelPiece", [h, length, across]() { makePieceCoro(h, length, across); });
}

// static
void WolfFloaterSeaTunnels::makePieceCoro(LLHandle<LLFloater> h, F32 length, F32 across)
{
    auto finish = [h](const std::string& text, bool error)
    {
        if (WolfFloaterSeaTunnels* f = dynamic_cast<WolfFloaterSeaTunnels*>(h.get()))
        {
            f->mMaking = false;
            f->setStatus(text, error);
            f->updateControls();
        }
    };
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn || !isAgentAvatarValid())
    {
        finish("You need to be in a region to make a piece.", true);
        return;
    }
    const LLUUID region = rgn->getRegionID();

    // Lying the way the avatar faces: the cylinder's own Z (its length) turned onto the ground's level (+90 degrees about
    // Y takes Z to X), then to the heading. LLQuaternion a * b applies a first (llquaternion.h; v * (a * b) = (v * a) * b,
    // as LLViewerObject::getRenderRotation's getRotation() * parent).
    LLVector3 fwd = gAgent.getAtAxis();
    fwd.mV[VZ] = 0.f;
    if (fwd.normVec() < 0.01f) fwd = LLVector3::x_axis;
    const F32 heading = atan2f(fwd.mV[VY], fwd.mV[VX]);
    const LLQuaternion rot = LLQuaternion(F_PI_BY_TWO, LLVector3::y_axis) * LLQuaternion(heading, LLVector3::z_axis);

    const F32 r = across * 0.5f;
    const F32 hollow = llclamp(1.f - WALL_M / r, 0.f, HOLLOW_MAX);
    const F32 inner = r * hollow;
    // its floor (the inside bottom of the tube) at the avatar's feet, its near end just in front of the avatar
    const LLVector3 me = rgn->getPosRegionFromAgent(gAgent.getPositionAgent());
    const F32 feet = me.mV[VZ] - gAgentAvatarp->getPelvisToFoot();
    LLVector3 centre = me + fwd * (length * 0.5f + 0.5f);
    centre.mV[VZ] = feet + inner;

    std::set<LLUUID> before;   // what is already there is never taken for the new piece
    for (S32 i = 0, n = gObjectList.getNumObjects(); i < n; ++i)
    {
        LLViewerObject* o = gObjectList.getObject(i);
        if (o && !o->isDead() && o->getRegion() == rgn && dist_vec(o->getPositionRegion(), centre) < 1.f) before.insert(o->getID());
    }
    // Source: wolfcavelights.cpp makePrim - send, then wait (15 s) for a root we own that was not there, at the centre.
    sendTube(centre, rot, LLVector3(across, across, length), hollow, rgn);
    LLViewerObject* made = nullptr;
    for (S32 t = 0; t < 150 && !made; ++t)
    {
        llcoro::suspendUntilTimeout(0.1f);
        LLViewerRegion* now = gAgent.getRegion();
        if (!now || now->getRegionID() != region)
        {
            finish("You left the region before the piece was made.", true);
            return;
        }
        for (S32 i = 0, n = gObjectList.getNumObjects(); i < n; ++i)
        {
            LLViewerObject* o = gObjectList.getObject(i);
            if (!o || o->isDead() || !o->isRoot() || o->getRegion() != now || before.count(o->getID())) continue;
            if (o->getPCode() != LL_PCODE_VOLUME || !o->permYouOwner()) continue;
            if (dist_vec(o->getPositionRegion(), centre) > 0.3f) continue;
            made = o;
            break;
        }
    }
    if (!made)
    {
        finish("The region didn't make the piece. You may not be allowed to build here, or the land may be full; the "
               "region's own message, if it sent one, is on your screen.", true);
        return;
    }
    sendName(made, PIECE_NAME);
    sendDescription(made, WolfDryVolumes::KEYWORD);
    WolfObjectProps::instance().note(made->getID(), PIECE_NAME, WolfDryVolumes::KEYWORD);   // our own viewer knows at once
    // Pale blue glass on every face (wolfcavelights.cpp dressFitting: setTE... then sendTEUpdate)
    for (S32 te = 0, faces = made->getNumTEs(); te < faces; ++te)
    {
        made->setTETexture((U8)te, IMG_WHITE);
        made->setTEColor((U8)te, LLColor4(0.78f, 0.9f, 1.f, 0.22f));
        made->setTEShiny((U8)te, 2);
    }
    made->sendTEUpdate();
    finish(llformat("Made a %.0f m piece in front of you. Move, turn and stretch it with Build, and Shift-drag to copy it "
                    "end to end.", length), false);
}

void WolfFloaterSeaTunnels::onSetAir(bool air)
{
    S32 changed = 0, wrong_shape = 0, not_yours = 0, not_ready = 0;
    LLObjectSelectionHandle sel = LLSelectMgr::getInstance()->getSelection();
    for (LLObjectSelection::iterator it = sel->begin(); it != sel->end(); ++it)
    {
        LLSelectNode* node = *it;
        LLViewerObject* o = node ? node->getObject() : nullptr;
        if (!o || o->isDead() || !o->getRegion() || o->isAvatar()) continue;
        WolfDryVolumes::Volume v;
        if (!WolfDryVolumes::volumeOf(o, v)) { ++wrong_shape; continue; }
        if (!o->permModify()) { ++not_yours; continue; }
        // the description the region last sent: the selection's (ObjectProperties on select), else the shared store's
        const WolfObjectProps::Props* known = WolfObjectProps::instance().get(o->getID());
        if (!node->mValid && !known) { ++not_ready; continue; }
        const std::string desc = node->mValid ? node->mDescription : known->mDescription;
        const std::string name = node->mValid ? node->mName : known->mName;
        const std::string next = withAir(desc, air);
        if (next == desc) continue;
        sendDescription(o, next);
        if (node->mValid) node->mDescription = next;
        WolfObjectProps::instance().note(o->getID(), name, next);
        ++changed;
    }
    std::string text = air ? llformat("Filled %d piece%s with air.", changed, changed == 1 ? "" : "s")
                           : llformat("Made %d piece%s ordinary again.", changed, changed == 1 ? "" : "s");
    if (wrong_shape) text += llformat(" %d left as they are: only cylinders, boxes, spheres and mesh tubes can hold air.", wrong_shape);
    if (not_yours) text += llformat(" %d you aren't allowed to change.", not_yours);
    if (not_ready) text += llformat(" %d still loading: try again in a moment.", not_ready);
    setStatus(text, changed == 0 && (wrong_shape || not_yours || not_ready));
}
