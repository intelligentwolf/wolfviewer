/**
 * @file wolffloatersoundeffects.cpp
 * [SOUND EFFECTS 2026-11-06] See wolffloatersoundeffects.h.
 */
#include "llviewerprecompiledheaders.h"

#include "wolffloatersoundeffects.h"

#include "llcombobox.h"
#include "llframetimer.h"
#include "llinventorymodel.h"
#include "lltextbox.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "wolfsoundeffects.h"

namespace
{
    const char* const OWN = "own";       // the list value of the resident's own sound
    constexpr F64 PENDING_SECS = 10.0;   // a sample still not decoded by then is dropped quietly
}

WolfFloaterSoundEffects::WolfFloaterSoundEffects(const LLSD& key) : LLFloater(key) {}

bool WolfFloaterSoundEffects::postBuild()
{
    mList = getChild<LLComboBox>("teleport_sound");
    mStatus = getChild<LLTextBox>("status");
    mList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPick(); });
    childSetAction("play_btn", [this](void*) { onPlay(); }, this);
    childSetAction("reset_btn", [this](void*) { onReset(); }, this);
    return true;
}

void WolfFloaterSoundEffects::onOpen(const LLSD& key)
{
    fillList();
    say("");
    // the chosen sound decoded now, so Play (and the next teleport) has it at once
    WolfSoundEffects::prepare(LLUUID(gSavedSettings.getString("UISndTeleportOut")));
}

void WolfFloaterSoundEffects::fillList()
{
    const LLUUID current(gSavedSettings.getString("UISndTeleportOut"));
    mList->removeall();
    for (const auto& t : WolfSoundEffects::teleportSounds())
    {
        mList->add(t.mLabel, LLSD(t.mKey));
    }
    // Any other sound (dropped here, or set in Preferences > Sounds) is the resident's own. A null UUID is "silent"
    // in Firestorm; here that is the Teleport sound tick box, so it shows as the standard sound.
    const WolfSoundEffects::TeleportSound* known = WolfSoundEffects::findTeleportSound(current);
    if (current.notNull() && !known)
    {
        const std::string name = gSavedSettings.getString("WolfSoundTeleportOwnName");
        LLStringUtil::format_map_t args;
        args["[NAME]"] = name;
        mList->add(name.empty() ? getString("own_sound_unnamed") : getString("own_sound", args), LLSD(OWN));
        mList->setValue(LLSD(OWN));
    }
    else
    {
        mList->setValue(LLSD(known ? known->mKey : "standard"));
    }
}

void WolfFloaterSoundEffects::onPick()
{
    const std::string key = mList->getValue().asString();
    if (key == OWN) { onPlay(); return; }   // already the setting
    for (const auto& t : WolfSoundEffects::teleportSounds())
    {
        if (key != t.mKey) continue;
        gSavedSettings.setString("UISndTeleportOut", t.mUuid);
        gSavedSettings.setBOOL("PlayModeUISndTeleportOut", true);   // choosing a sound means wanting to hear it
        say("");
        playWhenReady(LLUUID(t.mUuid));
        return;
    }
}

void WolfFloaterSoundEffects::onPlay()
{
    playWhenReady(LLUUID(gSavedSettings.getString("UISndTeleportOut")));
}

void WolfFloaterSoundEffects::playWhenReady(const LLUUID& id)
{
    if (id.isNull()) return;
    if (WolfSoundEffects::prepare(id))
    {
        mPending.setNull();
        WolfSoundEffects::preview(id, llclamp(gSavedSettings.getF32("WolfSoundTeleportVolume"), 0.f, 1.f));
        return;
    }
    mPending = id;
    mPendingUntil = LLFrameTimer::getElapsedSeconds() + PENDING_SECS;
    say(getString("loading"));
}

void WolfFloaterSoundEffects::draw()
{
    if (mPending.notNull())
    {
        if (WolfSoundEffects::prepare(mPending))
        {
            const LLUUID id = mPending;
            mPending.setNull();
            say("");
            WolfSoundEffects::preview(id, llclamp(gSavedSettings.getF32("WolfSoundTeleportVolume"), 0.f, 1.f));
        }
        else if (LLFrameTimer::getElapsedSeconds() > mPendingUntil)
        {
            mPending.setNull();
            say("");
        }
    }
    LLFloater::draw();
}

void WolfFloaterSoundEffects::onReset()
{
    for (S32 g = 0; g < WolfSoundEffects::GROUP_COUNT; ++g)
    {
        const auto grp = (WolfSoundEffects::Group)g;
        gSavedSettings.setBOOL(WolfSoundEffects::switchSetting(grp), true);
        gSavedSettings.setF32(WolfSoundEffects::volumeSetting(grp), 1.f);
    }
    gSavedSettings.setString("UISndTeleportOut", WolfSoundEffects::standardTeleportSound().asString());
    gSavedSettings.setString("WolfSoundTeleportOwnName", "");
    fillList();
    say(getString("reset_done"));
}

// A sound dragged from the resident's inventory onto any part of the window becomes the teleport sound (Luke Koda,
// ticket 568: "would like to be able to choose a sound ... look in my inventory for romulan tp as example").
// Source of the pattern: wolffloaterbugreport.cpp handleDragAndDrop (the window takes the drop before its children).
bool WolfFloaterSoundEffects::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type,
                                                void* cargo_data, EAcceptance* accept, std::string& tooltip_msg)
{
    if (cargo_type == DAD_SOUND && cargo_data)
    {
        const LLInventoryItem* item = static_cast<const LLInventoryItem*>(cargo_data);
        // Only the resident's own inventory, and only a sound whose asset the viewer is allowed to know: an item the
        // permissions hide arrives with a null asset id (LLViewerInventoryItem::getAssetUUID), and could not play.
        if (!gInventory.getItem(item->getUUID()))
        {
            *accept = ACCEPT_NO;
            return true;
        }
        if (item->getAssetUUID().isNull())
        {
            *accept = ACCEPT_NO;
            tooltip_msg = getString("no_asset");
            return true;
        }
        *accept = ACCEPT_YES_COPY_SINGLE;
        tooltip_msg = "Use this sound when you teleport";
        if (drop)
        {
            gSavedSettings.setString("UISndTeleportOut", item->getAssetUUID().asString());
            gSavedSettings.setString("WolfSoundTeleportOwnName", item->getName());
            gSavedSettings.setBOOL("PlayModeUISndTeleportOut", true);
            fillList();
            LLStringUtil::format_map_t args;
            args["[NAME]"] = item->getName();
            say(getString("dropped", args));
            playWhenReady(item->getAssetUUID());
        }
        return true;
    }
    if (cargo_type >= DAD_TEXTURE && cargo_type < DAD_COUNT && cargo_data)
    {
        // any other inventory item: say what is wanted rather than nothing
        *accept = ACCEPT_NO;
        tooltip_msg = getString("not_sound");
        return true;
    }
    return LLFloater::handleDragAndDrop(x, y, mask, drop, cargo_type, cargo_data, accept, tooltip_msg);
}

void WolfFloaterSoundEffects::say(const std::string& text)
{
    if (mStatus) mStatus->setText(text);
}
