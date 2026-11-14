/**
 * @file wolfx11selection.h
 * @brief Reading an X11 selection property, including the ICCCM INCR protocol.
 *
 * <WolfViewer 2026-11-14> Ticket 856: a 64,511-character script copied on a Linux desktop did
 * not paste into the script editor, with no message. The viewer's X11 clipboard code read the
 * selection property once and treated whatever it found as text. A selection owner that sends
 * a large selection incrementally (ICCCM 2.7.2, "INCR Properties") instead puts a property of
 * type INCR there, holding only a size estimate, and waits for the requestor to delete it before
 * sending the real data in pieces. The viewer never did that, so it read four bytes of a number
 * as text and the owner's transfer timed out.
 *
 * Owners that use INCR at script sizes include KWin's XWayland clipboard bridge (KDE Plasma on
 * Wayland), which goes incremental once the text fills its first 63 KiB chunk:
 *   Source: KDE kwin src/xwayland/transfer.cpp - static const uint32_t s_incrChunkSize = 63 * 1024;
 *           TransferWltoX::readWlSource() "first chunk full, but not yet at fd end -> go incremental"
 * GTK's X11 backend does the same when the data arrives in more than one write:
 *   Source: gtk gdk/x11/gdkselectionoutputstream-x11.c gdk_x11_selection_output_stream_perform_flush()
 *
 * Header-only and plain Xlib, with no viewer headers, so the offline test
 * (indra/llwindow/tests/wolfx11selection_test.cpp) builds this exact code against a bare X
 * server, and no CMake change is needed for llwindowsdl2.cpp to use it.
 */

#ifndef WOLF_X11_SELECTION_H
#define WOLF_X11_SELECTION_H

#if LL_X11

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>
#include <functional>
#include <string>

namespace wolfx11
{
    // Waits up to timeout_ms for the next X event delivered to this client and copies it into
    // `event`. Returns false when nothing arrived in time. The viewer passes one built on SDL's
    // event queue (llwindowsdl2.cpp); the offline test passes one built on XNextEvent.
    typedef std::function<bool(XEvent& event, unsigned int timeout_ms)> WaitForEvent;

    // How long to wait for each piece of an INCR transfer. The same 1 s the viewer already
    // allows the owner to answer XConvertSelection.
    // Source: llwindowsdl2.cpp grab_property() - `const auto end{ start + 1000 };`
    const unsigned int INCR_PIECE_TIMEOUT_MS = 1000;

    // The most an INCR transfer may bring in. Our own limit, not a protocol value: an owner that
    // keeps sending would otherwise hold the viewer and grow memory without end. 16 MiB is 64
    // times the largest text box in the viewer, the script editor's 262144 bytes
    // (skins/default/xui/en/panel_script_ed.xml max_length="262144"), and still a quick read.
    const size_t MAX_SELECTION_BYTES = 16 * 1024 * 1024;

    // And the longest a whole INCR transfer may take, so an owner that trickles pieces just
    // inside INCR_PIECE_TIMEOUT_MS cannot hold the viewer for minutes. Also our own limit: the
    // 300 KB offline test transfers (tests/wolfx11selection_test.cpp) finish in milliseconds.
    const unsigned int MAX_TRANSFER_MS = 10000;

    enum ReadResult
    {
        READ_OK,              // `bytes` holds the whole selection
        READ_NO_PROPERTY,     // the owner left no property (conversion refused)
        READ_NOT_TEXT,        // the property is not 8-bit data, so it cannot be text
        READ_INCR_TIMEOUT,    // an INCR transfer stopped before its zero-length end marker
        READ_TOO_LARGE,       // an INCR transfer went past MAX_SELECTION_BYTES
        READ_X_ERROR          // XGetWindowProperty failed
    };

    // Reads `property` on `window` after the owner's SelectionNotify, following INCR when the
    // owner chose it. The caller holds no Xlib lock across the call if its WaitForEvent pumps a
    // toolkit that takes one (SDL does: see maybe_lock_display() in llwindowsdl2.cpp); `lock` and
    // `unlock` bracket every direct Xlib call made here and may be empty.
    inline ReadResult readSelectionProperty(Display* display, Window window, Atom property,
                                            const WaitForEvent& wait_for_event,
                                            const std::function<void()>& lock,
                                            const std::function<void()>& unlock,
                                            std::string& bytes);

    inline const char* describe(ReadResult result)
    {
        switch (result)
        {
        case READ_OK:           return "read";
        case READ_NO_PROPERTY:  return "no property";
        case READ_NOT_TEXT:     return "not 8-bit text";
        case READ_INCR_TIMEOUT: return "incremental (INCR) transfer stopped before its end";
        case READ_TOO_LARGE:    return "incremental (INCR) transfer larger than the viewer takes";
        case READ_X_ERROR:      return "XGetWindowProperty failed";
        }
        return "unknown";
    }

    namespace detail
    {
        struct Property
        {
            Atom type = None;
            int format = 0;
            std::string data;   // format 8: the bytes; format 32: the first long, raw
        };

        // Reads the whole of `property` in two requests, as the Firestorm fix does: a zero-length
        // read for the size, then exactly that much. `remove` deletes it after the read, which in
        // an INCR transfer is the requestor's "send the next piece" signal (ICCCM 2.7.2).
        // Source: phoenix-firestorm 48282c7fea (FIRE-31976) llwindowsdl2.cpp getSelectionText()
        // Xlib: long_offset/long_length count 32-bit units; bytes_after counts bytes; the
        // server deletes only when the read leaves bytes_after == 0 (XGetWindowProperty(3)).
        inline bool readProperty(Display* display, Window window, Atom atom, bool remove,
                                 Property& out)
        {
            Atom type = None;
            int format = 0;
            unsigned long items = 0, after = 0;
            unsigned char* data = nullptr;

            if (XGetWindowProperty(display, window, atom, 0, 0, False, AnyPropertyType,
                                   &type, &format, &items, &after, &data) != Success)
            {
                return false;
            }
            if (data)
            {
                XFree(data);
                data = nullptr;
            }
            out = Property();
            out.type = type;
            out.format = format;
            if (type == None)
            {
                return true;   // no such property
            }

            const unsigned long units = (after + 3) / 4;
            if (XGetWindowProperty(display, window, atom, 0, (long)(units > LONG_MAX ? LONG_MAX : units),
                                   remove ? True : False, AnyPropertyType,
                                   &type, &format, &items, &after, &data) != Success)
            {
                return false;
            }
            out.type = type;
            out.format = format;
            if (data && items)
            {
                // Xlib hands format 32 back as an array of C long, format 16 as short.
                const size_t element = (format == 32) ? sizeof(long) : (format == 16) ? sizeof(short) : 1;
                out.data.assign(reinterpret_cast<const char*>(data), items * element);
            }
            if (data)
            {
                XFree(data);
            }
            return true;
        }

        inline void call(const std::function<void()>& f)
        {
            if (f)
            {
                f();
            }
        }
    }

    inline ReadResult readSelectionProperty(Display* display, Window window, Atom property,
                                            const WaitForEvent& wait_for_event,
                                            const std::function<void()>& lock,
                                            const std::function<void()>& unlock,
                                            std::string& bytes)
    {
        bytes.clear();

        detail::call(lock);
        const Atom incr = XInternAtom(display, "INCR", False);
        detail::Property first;
        // Not deleted yet: for an ordinary transfer deleting it is the requestor's courtesy, for
        // INCR deleting it is the start signal, and we only know which after reading the type.
        const bool read_ok = detail::readProperty(display, window, property, false, first);
        detail::call(unlock);

        if (!read_ok)
        {
            return READ_X_ERROR;
        }
        if (first.type == None)
        {
            return READ_NO_PROPERTY;
        }

        if (first.type != incr)
        {
            detail::call(lock);
            XDeleteProperty(display, window, property);   // ICCCM 2.4: the requestor deletes it
            XFlush(display);
            detail::call(unlock);
            if (first.format != 8)
            {
                return READ_NOT_TEXT;
            }
            bytes.swap(first.data);
            return READ_OK;
        }

        // INCR (ICCCM 2.7.2): the property holds a lower bound on the size. Deleting it asks the
        // owner for the first piece; each piece arrives as a PropertyNotify(NewValue) on our
        // property, we read it and delete it (asking for the next), and a zero-length piece ends
        // the transfer. Our window must select PropertyChangeMask: SDL's X11 windows do
        // (SDL 2.28 SDL_x11window.c X11_CreateWindow XSelectInput(... PropertyChangeMask ...)).
        if (first.data.size() >= sizeof(long))
        {
            long estimate = 0;
            memcpy(&estimate, first.data.data(), sizeof(long));
            if (estimate > 0)
            {
                bytes.reserve(std::min((size_t)estimate, MAX_SELECTION_BYTES));
            }
        }

        detail::call(lock);
        XDeleteProperty(display, window, property);
        XFlush(display);
        detail::call(unlock);

        int piece_format = 8;
        const auto transfer_deadline = std::chrono::steady_clock::now()
                                     + std::chrono::milliseconds(MAX_TRANSFER_MS);
        for (;;)
        {
            XEvent event;
            bool got_piece = false;
            // Wait for the owner to write the next piece, ignoring everything else (including
            // the PropertyNotify(Delete) our own deletions cause). The deadline is for the
            // piece, not for each event, so a stream of mouse moves cannot keep a dead
            // transfer waiting for ever.
            const auto deadline = std::min(transfer_deadline, std::chrono::steady_clock::now()
                                           + std::chrono::milliseconds(INCR_PIECE_TIMEOUT_MS));
            while (!got_piece)
            {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
                if (left <= 0 || !wait_for_event(event, (unsigned int)left))
                {
                    bytes.clear();
                    return READ_INCR_TIMEOUT;
                }
                got_piece = event.type == PropertyNotify
                         && event.xproperty.window == window
                         && event.xproperty.atom == property
                         && event.xproperty.state == PropertyNewValue;
            }

            detail::Property piece;
            detail::call(lock);
            const bool piece_ok = detail::readProperty(display, window, property, true, piece);
            XFlush(display);   // the deletion is the owner's cue for the next piece
            detail::call(unlock);

            if (!piece_ok)
            {
                bytes.clear();
                return READ_X_ERROR;
            }
            if (piece.type == None)
            {
                continue;   // a stale notification for a property already read
            }
            if (piece.data.empty())
            {
                break;      // the zero-length end marker
            }
            piece_format = piece.format;
            if (bytes.size() + piece.data.size() > MAX_SELECTION_BYTES)
            {
                bytes.clear();
                return READ_TOO_LARGE;
            }
            bytes.append(piece.data);
        }

        if (piece_format != 8)
        {
            bytes.clear();
            return READ_NOT_TEXT;
        }
        return READ_OK;
    }
}

#endif // LL_X11
#endif // WOLF_X11_SELECTION_H
