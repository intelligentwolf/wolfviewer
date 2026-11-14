/**
 * @file wolfpastestatus.h
 * @brief Did the last clipboard read fail while reading text that was on offer?
 *
 * <WolfViewer 2026-11-14> Ticket 856. LLWindow::pasteTextFromClipboard() returns false both when
 * there is simply no text (an image or files on the clipboard, or nothing) and when text was on
 * offer but could not be read. Only the second deserves a message to the resident
 * (LLTextEditor/LLLineEditor::pasteHelper show WolfPasteFailed). The platform code marks it here:
 * - Linux/X11 (llwindowsdl2.cpp getSelectionText): the owner answered with a property and then the
 *   transfer broke (INCR stopped, too large, not 8-bit text, X error).
 * - Windows (llwindowwin32.cpp pasteTextFromClipboard): CF_UNICODETEXT was available but
 *   OpenClipboard / GetClipboardData / GlobalLock failed.
 * - macOS and native Wayland do not mark it: their reads return false for an empty string too
 *   (llwindowmacosx.cpp `dst != L""`, llwindowsdl2.cpp sdl_text `!dst.empty()`; SDL 2.28
 *   SDL_GetClipboardText returns "" on failure), so a failure cannot be told from "nothing there".
 *
 * Header-only (C++17 function-local static) so no CMake change is needed; llui and llwindow are
 * linked into the one viewer binary, so they share the one flag. Main thread only.
 */

#ifndef WOLF_PASTE_STATUS_H
#define WOLF_PASTE_STATUS_H

namespace wolfpaste
{
    inline bool& lastReadFailedFlag()
    {
        static bool failed = false;
        return failed;
    }

    inline void clearLastReadFailed() { lastReadFailedFlag() = false; }
    inline void setLastReadFailed() { lastReadFailedFlag() = true; }
    inline bool lastReadFailed() { return lastReadFailedFlag(); }
}

#endif // WOLF_PASTE_STATUS_H
