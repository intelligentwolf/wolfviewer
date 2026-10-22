/**
 * @file wolfvoicediag.h
 * @brief WolfViewer voice debug mode: a short in-memory list of voice events (2026-10-22).
 *
 * Off for everyone unless the Wolf Chat service says this avatar has voice debug on (php/wolfchat.php
 * voice_debug: the resident ticked it, or we switched it on for them). While off, note() returns at once and
 * nothing is kept or sent. While on, the last MAX_EVENTS voice events (channel activate / leave and why,
 * connection states, join sent, mic changes, settings suspend / resume) are kept in memory and go with the
 * Wolf Chat room reports (WolfChat::sendVoiceReport). No chat text, no audio.
 */

#ifndef WOLF_VOICEDIAG_H
#define WOLF_VOICEDIAG_H

#include <string>
#include "llsd.h"

namespace WolfVoiceDiag
{
    /** True while voice debug mode is on for this avatar. */
    bool enabled();
    void setEnabled(bool on);
    /** Records one event (time-stamped, at most 160 characters) when enabled; does nothing otherwise. */
    void note(const std::string& event);
    /** The kept events, oldest first, as an array of strings. */
    LLSD events();
    void clear();
}

#endif
