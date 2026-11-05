// [VR KEYBOARD 2026-11-05] xkeys: REAL keyboard input for a test viewer on its own X server (the e2e keyboard check,
// e2e_keyboard.mjs). XTEST key events enter the X server's own input path, exactly as a hardware key does, and reach the
// viewer as X KeyPress / KeyRelease (and the input method's text) through SDL's X11 backend - the path every key of a
// WolfViewer window takes on Linux (the viewer runs on SDL's x11 driver: "SDL video driver: x11" in its log, under
// XWayland on Wayland desktops). Not LEAP's key injection, which calls the viewer's key handlers directly and so never
// sees window focus or SDL's state. Run it against a private X server (Xwayland -geometry ..., one per test session),
// never the desktop's, so the test never takes the keyboard from the person at the PC.
//   xkeys focus <pid>            give X input focus to the top-level window whose _NET_WM_PID is <pid> (prints it)
//   xkeys focused                print the window with X input focus
//   xkeys hold <keysym> <ms>     press, hold <ms>, release (keysym names as XStringToKeysym: Up, Prior, w, Return)
//   xkeys tap <keysym>           press and release
//   xkeys type <text>            type letters, digits, space and . , - (Shift for capitals)
// Build: cc -O2 -o xkeys xkeys.c -lX11 -lXtst
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Window find_pid(Display* d, Window w, unsigned long pid, Atom a)
{
    Atom type; int fmt; unsigned long n, after; unsigned char* prop = NULL;
    if (XGetWindowProperty(d, w, a, 0, 1, False, XA_CARDINAL, &type, &fmt, &n, &after, &prop) == Success && prop)
    {
        const unsigned long v = n ? *(unsigned long*)prop : 0;
        XFree(prop);
        if (n && v == pid)
        {
            XWindowAttributes wa;
            if (XGetWindowAttributes(d, w, &wa) && wa.map_state == IsViewable) return w;
        }
    }
    Window root, parent, *kids = NULL; unsigned int nk = 0;
    if (!XQueryTree(d, w, &root, &parent, &kids, &nk)) return 0;
    Window found = 0;
    for (unsigned int i = 0; i < nk && !found; ++i) found = find_pid(d, kids[i], pid, a);
    if (kids) XFree(kids);
    return found;
}

static void key(Display* d, KeySym ks, Bool down)
{
    const KeyCode kc = XKeysymToKeycode(d, ks);
    if (!kc) { fprintf(stderr, "no keycode for keysym 0x%lx\n", ks); exit(3); }
    XTestFakeKeyEvent(d, kc, down, CurrentTime);
    XFlush(d);
}

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: see the head of xkeys.c\n"); return 2; }
    Display* d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "cannot open display\n"); return 2; }
    int ev, er, maj, min;
    if (!XTestQueryExtension(d, &ev, &er, &maj, &min)) { fprintf(stderr, "no XTEST\n"); return 2; }
    const char* op = argv[1];
    if (!strcmp(op, "focus") && argc == 3)
    {
        const Window w = find_pid(d, DefaultRootWindow(d), strtoul(argv[2], NULL, 10), XInternAtom(d, "_NET_WM_PID", True));
        if (!w) { fprintf(stderr, "no mapped window for pid %s\n", argv[2]); return 4; }
        XRaiseWindow(d, w);
        XSetInputFocus(d, w, RevertToParent, CurrentTime);
        XSync(d, False);
        printf("0x%lx\n", w);
    }
    else if (!strcmp(op, "focused"))
    {
        Window w; int rev;
        XGetInputFocus(d, &w, &rev);
        printf("0x%lx\n", w);
    }
    else if (!strcmp(op, "hold") && argc == 4)
    {
        const KeySym ks = XStringToKeysym(argv[2]);
        if (ks == NoSymbol) { fprintf(stderr, "unknown keysym %s\n", argv[2]); return 2; }
        key(d, ks, True);
        usleep((useconds_t)atoi(argv[3]) * 1000);
        key(d, ks, False);
    }
    else if (!strcmp(op, "tap") && argc == 3)
    {
        const KeySym ks = XStringToKeysym(argv[2]);
        if (ks == NoSymbol) { fprintf(stderr, "unknown keysym %s\n", argv[2]); return 2; }
        key(d, ks, True); usleep(40000); key(d, ks, False);
    }
    else if (!strcmp(op, "type") && argc == 3)
    {
        for (const char* c = argv[2]; *c; ++c)
        {
            KeySym ks; Bool shift = False;
            if (*c >= 'A' && *c <= 'Z') { ks = XK_a + (*c - 'A'); shift = True; }
            else if ((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9')) ks = (KeySym)*c;
            else if (*c == ' ') ks = XK_space;
            else if (*c == '.') ks = XK_period;
            else if (*c == ',') ks = XK_comma;
            else if (*c == '-') ks = XK_minus;
            else { fprintf(stderr, "cannot type '%c'\n", *c); return 2; }
            if (shift) key(d, XK_Shift_L, True);
            key(d, ks, True); usleep(30000); key(d, ks, False);
            if (shift) key(d, XK_Shift_L, False);
            usleep(60000);
        }
    }
    else { fprintf(stderr, "bad arguments\n"); return 2; }
    XCloseDisplay(d);
    return 0;
}
