/**
 * @file wolffloatersoundeffects.h
 * [SOUND EFFECTS 2026-11-06] Wolf World > Sound Effects... - a switch and a volume for each group of Wolf sounds, and the
 * teleport sound (WolfViewer's own, the standard one, or a sound dropped from inventory). See wolfsoundeffects.h and
 * floater_wolf_sound_effects.xml. The tick boxes and sliders are bound to their settings in the XML; this class does
 * the teleport sound list, Play, the drop and "Back to normal".
 */
#ifndef WOLF_FLOATER_SOUND_EFFECTS_H
#define WOLF_FLOATER_SOUND_EFFECTS_H

#include "llfloater.h"

class LLComboBox;
class LLTextBox;

class WolfFloaterSoundEffects : public LLFloater
{
public:
    WolfFloaterSoundEffects(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;
    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type, void* cargo_data,
                           EAcceptance* accept, std::string& tooltip_msg) override;

private:
    void fillList();
    void onPick();
    void onPlay();
    void onReset();
    /** Plays the sound now if it is ready, else as soon as it is (draw() checks). */
    void playWhenReady(const LLUUID& id);
    void say(const std::string& text);

    LLComboBox* mList = nullptr;
    LLTextBox* mStatus = nullptr;
    LLUUID mPending;           ///< a sample waiting for its decoding
    F64 mPendingUntil = 0.0;   ///< give up on it then
};

#endif
