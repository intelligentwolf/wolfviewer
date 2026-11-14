/**
 * @file wolfx11selection_test.cpp
 * @brief Offline test of wolfx11selection.h against a real X server (ticket 856).
 *
 * Not part of the CMake build: it needs an X server of its own. Run it on a private Xvfb:
 *   Xvfb :97 -screen 0 640x480x24 -nolisten tcp &
 *   g++ -std=c++17 -DLL_X11=1 -I indra/llwindow indra/llwindow/tests/wolfx11selection_test.cpp \
 *       -o /some/dir/wolfx11selection_test -lX11 -lpthread
 *   DISPLAY=:97 /some/dir/wolfx11selection_test
 * or, to read whatever real program owns CLIPBOARD on that display (a GTK or Qt app):
 *   DISPLAY=:97 /some/dir/wolfx11selection_test --external > pasted.txt
 *
 * One thread owns CLIPBOARD on its own X connection and answers like KWin's XWayland bridge:
 * text that fills a 63 KiB chunk goes by INCR in 63 KiB pieces, anything smaller in one go.
 * Source: KDE kwin src/xwayland/transfer.cpp - s_incrChunkSize = 63 * 1024, TransferWltoX.
 * The main thread is the viewer: XConvertSelection, wait for SelectionNotify, then
 * wolfx11::readSelectionProperty - the same code llwindowsdl2.cpp getSelectionText() runs.
 * It also runs the read the viewer had before (copied from w106-beta llwindowsdl2.cpp
 * getSelectionText()) to show that read was the fault.
 */

#include "wolfx11selection.h"

#include <X11/Xutil.h>
#include <poll.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace
{
    const size_t KWIN_CHUNK = 63 * 1024;   // Source: kwin transfer.cpp s_incrChunkSize

    struct Owner
    {
        std::string text;
        bool stall_after_first_piece = false;   // owner that dies mid-transfer
        std::atomic<bool> ready { false };
        std::atomic<bool> quit { false };
        std::atomic<int> incr_transfers { 0 };

        void run()
        {
            Display* d = XOpenDisplay(nullptr);
            Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1, 1, 0, 0, 0);
            Atom clipboard = XInternAtom(d, "CLIPBOARD", False);
            Atom utf8 = XInternAtom(d, "UTF8_STRING", False);
            Atom incr = XInternAtom(d, "INCR", False);
            XSetSelectionOwner(d, clipboard, w, CurrentTime);
            XSync(d, False);
            ready = true;

            Window incr_win = None;
            Atom incr_prop = None;
            size_t sent = 0;
            bool end_sent = false;

            while (!quit)
            {
                if (!XPending(d))
                {
                    pollfd p { ConnectionNumber(d), POLLIN, 0 };
                    poll(&p, 1, 20);
                    continue;
                }
                XEvent e;
                XNextEvent(d, &e);
                if (e.type == SelectionRequest)
                {
                    XSelectionRequestEvent& r = e.xselectionrequest;
                    XSelectionEvent reply {};
                    reply.type = SelectionNotify;
                    reply.display = d;
                    reply.requestor = r.requestor;
                    reply.selection = r.selection;
                    reply.target = r.target;
                    reply.property = r.property;
                    reply.time = r.time;
                    if (r.target != utf8)
                    {
                        reply.property = None;
                    }
                    else if (text.size() < KWIN_CHUNK)
                    {
                        XChangeProperty(d, r.requestor, r.property, utf8, 8, PropModeReplace,
                                        (const unsigned char*)text.data(), (int)text.size());
                    }
                    else
                    {
                        // INCR: watch the requestor's property for deletions, announce the size
                        XSelectInput(d, r.requestor, PropertyChangeMask);
                        long size = (long)text.size();
                        XChangeProperty(d, r.requestor, r.property, incr, 32, PropModeReplace,
                                        (const unsigned char*)&size, 1);
                        incr_win = r.requestor;
                        incr_prop = r.property;
                        sent = 0;
                        end_sent = false;
                        ++incr_transfers;
                    }
                    XSendEvent(d, r.requestor, False, NoEventMask, (XEvent*)&reply);
                    XFlush(d);
                }
                else if (e.type == PropertyNotify && e.xproperty.state == PropertyDelete
                         && e.xproperty.window == incr_win && e.xproperty.atom == incr_prop && !end_sent)
                {
                    if (stall_after_first_piece && sent > 0)
                    {
                        continue;   // never sends the rest
                    }
                    const size_t n = std::min(KWIN_CHUNK, text.size() - sent);
                    XChangeProperty(d, incr_win, incr_prop, utf8, 8, PropModeReplace,
                                    (const unsigned char*)text.data() + sent, (int)n);
                    sent += n;
                    if (n == 0)
                    {
                        end_sent = true;   // the zero-length piece ends it
                        XSelectInput(d, incr_win, NoEventMask);
                    }
                    XFlush(d);
                }
            }
            XDestroyWindow(d, w);
            XCloseDisplay(d);
        }
    };

    bool waitForEvent(Display* d, XEvent& e, unsigned int timeout_ms)
    {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!XPending(d))
        {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
            if (left <= 0)
            {
                return false;
            }
            pollfd p { ConnectionNumber(d), POLLIN, 0 };
            poll(&p, 1, (int)left);
        }
        XNextEvent(d, &e);
        return true;
    }

    // The viewer's grab_property(): ask, then wait up to 1 s for our SelectionNotify.
    bool convert(Display* d, Window w, Atom property)
    {
        Atom clipboard = XInternAtom(d, "CLIPBOARD", False);
        Atom utf8 = XInternAtom(d, "UTF8_STRING", False);
        XDeleteProperty(d, w, property);
        XConvertSelection(d, clipboard, utf8, property, w, CurrentTime);
        XFlush(d);
        XEvent e;
        while (waitForEvent(d, e, 1000))
        {
            if (e.type == SelectionNotify && e.xselection.requestor == w)
            {
                return e.xselection.property != None;
            }
        }
        return false;
    }

    // The read w106-beta llwindowsdl2.cpp getSelectionText(Atom, Atom, LLWString&) did (before
    // this fix), with utf8str_to_wstring left out: it returned the property bytes as the text.
    std::string oldRead(Display* d, Window w, Atom property)
    {
        Atom type;
        int format {};
        unsigned long len {}, size {};
        unsigned char* data = nullptr;
        XGetWindowProperty(d, w, property, 0, 0, False, AnyPropertyType, &type, &format, &len, &size, &data);
        XFree(data);
        unsigned long remaining {};
        XGetWindowProperty(d, w, property, 0, size, False, AnyPropertyType, &type, &format, &len, &remaining, &data);
        std::string text;
        if (data && len)
        {
            text = reinterpret_cast<const char*>(data);
            XFree(data);
        }
        return text;
    }

    int failures = 0;

    void check(bool ok, const char* what)
    {
        std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok)
        {
            ++failures;
        }
    }

    std::string makeScript(size_t chars, bool with_multibyte)
    {
        // LSL-looking lines; with_multibyte puts a "★" (3 bytes in UTF-8) in every 40th line
        std::string s;
        size_t n = 0, line = 0;
        while (n < chars)
        {
            std::string l = "    llOwnerSay(\"line " + std::to_string(line) + "\");";
            if (with_multibyte && line % 40 == 0)
            {
                l += " // \xE2\x98\x85";
            }
            l += "\n";
            for (size_t i = 0; i < l.size() && n < chars; )
            {
                // copy one whole UTF-8 character
                size_t len = ((unsigned char)l[i] >= 0xE0) ? 3 : 1;
                s.append(l, i, len);
                i += len;
                ++n;
            }
            ++line;
        }
        return s;
    }

    size_t utf8Chars(const std::string& s)
    {
        size_t n = 0;
        for (unsigned char c : s)
        {
            n += (c & 0xC0) != 0x80;
        }
        return n;
    }

    void runCase(const char* name, const std::string& text, bool stall, bool expect_ok, bool expect_incr)
    {
        std::printf("\n== %s: %zu characters, %zu bytes\n", name, utf8Chars(text), text.size());
        Owner owner;
        owner.text = text;
        owner.stall_after_first_piece = stall;
        std::thread t([&owner] { owner.run(); });
        while (!owner.ready)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        Display* d = XOpenDisplay(nullptr);
        Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1, 1, 0, 0, 0);
        XSelectInput(d, w, PropertyChangeMask);   // as SDL's X11 windows do
        Atom property = XInternAtom(d, "FS_PASTE_BUFFER", False);

        // The old read, for the record
        if (expect_incr && !stall)
        {
            if (convert(d, w, property))
            {
                const std::string old = oldRead(d, w, property);
                std::printf("   old read gave %zu bytes (expected %zu)\n", old.size(), text.size());
                check(old != text, "the read before the fix does NOT get the text (the ticket 856 fault)");
            }
            // Let the owner's abandoned INCR transfer go: start a fresh request below.
            XDeleteProperty(d, w, property);
            XSync(d, False);
        }

        const int incr_before = owner.incr_transfers;
        std::string got;
        wolfx11::ReadResult result = wolfx11::READ_NO_PROPERTY;
        if (convert(d, w, property))
        {
            result = wolfx11::readSelectionProperty(
                d, w, property,
                [d](XEvent& e, unsigned int ms) { return waitForEvent(d, e, ms); },
                nullptr, nullptr, got);
        }
        std::printf("   new read: %s, %zu bytes\n", wolfx11::describe(result), got.size());
        if (expect_ok)
        {
            check(result == wolfx11::READ_OK && got == text, "the new read gets every byte of the text");
        }
        else
        {
            check(result == wolfx11::READ_INCR_TIMEOUT && got.empty(),
                  "an owner that stops mid-transfer gives a clean failure, not part of the text");
        }
        check((owner.incr_transfers > incr_before) == expect_incr,
              expect_incr ? "the owner used INCR" : "the owner sent it in one go");

        owner.quit = true;
        t.join();
        XDestroyWindow(d, w);
        XCloseDisplay(d);
    }
}

int main(int argc, char** argv)
{
    XInitThreads();
    if (!XOpenDisplay(nullptr))
    {
        std::printf("no X display\n");
        return 2;
    }
    if (argc > 1 && std::string(argv[1]) == "--external")
    {
        Display* d = XOpenDisplay(nullptr);
        Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1, 1, 0, 0, 0);
        XSelectInput(d, w, PropertyChangeMask);
        Atom property = XInternAtom(d, "FS_PASTE_BUFFER", False);
        std::string got;
        wolfx11::ReadResult result = wolfx11::READ_NO_PROPERTY;
        if (convert(d, w, property))
        {
            result = wolfx11::readSelectionProperty(
                d, w, property,
                [d](XEvent& e, unsigned int ms) { return waitForEvent(d, e, ms); },
                nullptr, nullptr, got);
        }
        std::fprintf(stderr, "external owner: %s, %zu bytes\n", wolfx11::describe(result), got.size());
        std::fwrite(got.data(), 1, got.size(), stdout);
        return result == wolfx11::READ_OK ? 0 : 1;
    }
    runCase("small script", makeScript(2000, false), false, true, false);
    runCase("ticket 856 size, plain", makeScript(64511, false), false, true, false);
    runCase("ticket 856 size, some symbols", makeScript(64511, true), false, true, true);
    runCase("70,000 characters", makeScript(70000, false), false, true, true);
    runCase("300,000 characters", makeScript(300000, true), false, true, true);
    runCase("owner stops after the first piece", makeScript(100000, false), true, false, true);
    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
