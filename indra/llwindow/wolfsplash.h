/**
 * @file wolfsplash.h
 * @brief WolfViewer's start-up splash: one software-drawn picture shared by Windows, Linux and macOS.
 *
 * [WOLF SPLASH 2026-10-21] Paul: "windows when it starts opens a big blank screen and hidden is the 'I'm loading shaders'
 * i'd rather just display a really posh splash screen" ... "all versions should show it".
 *
 * Why it was hidden (Windows): LLSplashScreenWin32 was the stock "SPLASHSCREEN" dialog (res/viewerRes.rc:66, a 264x34
 * DLU box, not topmost) created on the MAIN thread (llwindowwin32.cpp LLSplashScreenWin32::showImpl), so nothing pumped
 * its messages while LLViewerWindow's constructor compiled the shaders (llviewerwindow.cpp setShaders right after
 * createWindow) and it stopped painting; and LLAppViewer::initWindow maximized the hidden main window
 * (llappviewer.cpp maximize() -> llwindowwin32.cpp maximize: SetWindowPlacement SW_MAXIMIZE, which also SHOWS it), so a
 * big blank window covered it until login. On Linux there was no splash at all (llwindow.cpp LLSplashScreen::show has
 * no SDL branch) and the SDL window was created visible (llwindowsdl2.cpp SDL_CreateWindow without SDL_WINDOW_HIDDEN).
 *
 * The picture is drawn here, with FreeType and libjpeg (the viewer's own libraries), into a premultiplied BGRA buffer;
 * each platform only puts that buffer in a borderless window that keeps repainting on its own (a thread on Windows, a
 * small helper process of the viewer itself on Linux and macOS, whose UI must run on a main thread the viewer blocks).
 */

#ifndef WOLF_SPLASH_H
#define WOLF_SPLASH_H

#include <string>
#include <vector>
#include <cstdint>

struct WolfSplashConfig
{
    std::string art_path;       // the background photograph (JPEG)
    std::string title_font;     // display face for the name (Cinzel)
    std::string italic_font;    // the release name (serif italic)
    std::string ui_font;        // status and version (sans)
    std::string title;          // "WOLFVIEWER"
    std::string subtitle;       // the release name, e.g. "Welcome Home"
    std::string version;        // e.g. "Version 7.2.4 (75)"
    std::string status;         // the first status line
    int hint_x = 0;             // a screen point on the monitor the viewer will open on
    int hint_y = 0;
    bool has_hint = false;
};

class WolfSplash
{
public:
    // Logical size of the splash, before the display scale is applied.
    static constexpr int LOGICAL_W = 640;
    static constexpr int LOGICAL_H = 360;

    // What the application wants shown (set before LLSplashScreen::show, read by every platform).
    static void setConfig(const WolfSplashConfig& cfg);
    static const WolfSplashConfig& getConfig();
    static bool hasConfig();

    // The helper-process command line (Linux, macOS): the config packed into argv and back.
    static std::vector<std::string> toArgs(const WolfSplashConfig& cfg);
    static bool fromArgs(int argc, char** argv, int first, WolfSplashConfig& cfg);
};

#if !defined(_WIN32)
// Linux and macOS: the splash is a helper process - this same executable started with --wolf-splash (the platform's
// main() hands it to wolf_splash_helper_main) - because their window systems want UI on a main thread, and the
// viewer's is busy compiling shaders. Lines over a socket: "S <status>" sets the status, "F" fades it out; the socket
// closing (the viewer quit or crashed) ends it too. The viewer never waits for it.
class WolfSplashChild
{
public:
    WolfSplashChild() = default;
    ~WolfSplashChild() { stop(); }
    WolfSplashChild(const WolfSplashChild&) = delete;
    WolfSplashChild& operator=(const WolfSplashChild&) = delete;
    bool start(const std::string& exe_path);    // exe_path: this executable
    void status(const std::string& text);
    void stop();                                // fade out and let go (reaped on a detached thread)
    bool running() const { return mFd >= 0; }
    // the helper side: read what the viewer sent so far (non-blocking stdin); false once the viewer is gone
    static bool readCommands(std::string& buffer, std::string& status, bool& status_changed, bool& fade);
private:
    int mFd = -1;
    int mPid = -1;
};
#endif

// One splash picture at one pixel size. Not thread-safe: one renderer per thread / process.
class WolfSplashRenderer
{
public:
    WolfSplashRenderer();
    ~WolfSplashRenderer();
    WolfSplashRenderer(const WolfSplashRenderer&) = delete;              // owns FreeType handles
    WolfSplashRenderer& operator=(const WolfSplashRenderer&) = delete;

    // Builds everything that does not move (photo, shading, frame, name, release, version) for a w x h pixel window
    // at display scale `scale` (1.0 = 96 dpi). rounded: transparent rounded corners (only where the window can be
    // per-pixel transparent - Windows layered windows, macOS). Missing art or fonts are drawn around, never fatal.
    void init(const WolfSplashConfig& cfg, int w, int h, float scale, bool rounded);

    void setStatus(const std::string& status);

    // The current frame, premultiplied BGRA, w*h pixels, row-major from the top. t = seconds since shown (animates
    // the progress shimmer).
    void render(uint32_t* out, double t);

    int width() const { return mW; }
    int height() const { return mH; }

private:
    struct Face;
    void drawText(std::vector<uint32_t>& dst, Face* face, float px, float x, float baseline, const std::string& utf8,
                  uint32_t top_rgb, uint32_t bottom_rgb, float alpha, float spacing, bool shadow);
    Face* loadFace(const std::string& path);

    int mW = 0, mH = 0;
    float mScale = 1.f;
    bool mRounded = false;
    std::vector<uint32_t> mStatic;      // premultiplied BGRA
    std::vector<uint8_t> mMask;         // corner coverage 0..255 (rounded only)
    std::string mStatus;
    void* mFT = nullptr;                // FT_Library
    Face* mTitle = nullptr;
    Face* mItalic = nullptr;
    Face* mUI = nullptr;
};

#endif // WOLF_SPLASH_H
