/**
 * @file wolfgrid.h
 * @brief WolfViewer: is this session on Wolf Territories, and where are its own services?
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

#ifndef WOLF_GRID_H
#define WOLF_GRID_H

#include "llviewernetwork.h"
#include "lfsimfeaturehandler.h"   // <WolfViewer 2026-10-06/> the region's own grid (isOnWolfTerritories)
#include "llagent.h"                // <WolfViewer 2026-10-09/> isOnWolfTerritoriesConfirmed: gAgent.getRegion()
#include "llviewerregion.h"
// <WolfViewer 2026-09-27> makeVerifiedHttpOptions() below.
#include "httpoptions.h"
// </WolfViewer 2026-09-27>

#include "llfloater.h"
#include "llpanel.h"

// The features that talk to Wolf Territories' OWN servers — speech to text, text to speech,
// screen sharing — are offered on Wolf Territories only. Every one of them checks here, so a
// user of this viewer on another grid never sees a button that would send their audio or
// their screen to a server that is not that grid's.
namespace WolfGrid
{
    // Source: app_settings/grids.xml — the shipped grid entry has key
    // "grid.wolfterritories.org:8002" and grid_login_id "wolfterritories"
    // (llviewernetwork.cpp:42 GRID_ID_VALUE = "grid_login_id"; LLGridManager::getGridId()
    // returns that value for the current grid, getGrid() the key).
    // <WolfViewer 2026-10-06> host of a grid address ("http://grid.wolfterritories.org:8002",
    // "grid.wolfterritories.org:8002", ...) is wolfterritories.org or a name under it. Scheme,
    // port and path are ignored. Split out of isWolfTerritories() so the region's grid is held
    // to the same exact-host rule.
    inline bool isWolfHost(std::string host)
    {
        LLStringUtil::toLower(host);
        const size_t scheme = host.find("://");
        if (scheme != std::string::npos)
        {
            host.erase(0, scheme + 3);
        }
        const size_t host_end = host.find_first_of(":/");
        if (host_end != std::string::npos)
        {
            host.erase(host_end);
        }
        static const std::string WOLF_DOMAIN = "wolfterritories.org";
        static const std::string WOLF_SUFFIX = ".wolfterritories.org";
        return host == WOLF_DOMAIN
            || (host.size() > WOLF_SUFFIX.size()
                && host.compare(host.size() - WOLF_SUFFIX.size(), WOLF_SUFFIX.size(), WOLF_SUFFIX) == 0);
    }
    // </WolfViewer 2026-10-06>

    inline bool isWolfTerritories()
    {
        LLGridManager* gm = LLGridManager::getInstance();
        if (!gm)
        {
            return false;
        }
        if (gm->getGridId() == "wolfterritories")
        {
            return true;
        }
        // <WolfViewer 2026-09-27> Was a substring search, which "wolfterritories.org.evil.com"
        // or "notwolfterritories.org" also satisfied - and a positive answer makes the viewer
        // send the session id to Wolf services and offer Wolf-only features. Now the grid's HOST
        // must be wolfterritories.org or a name under it; scheme, port and path are ignored, so
        // "grid.wolfterritories.org:8002" (grids.xml, llviewernetwork.h MAINGRID) and any
        // hand-added variant of it still match.
        //return gm->getGrid().find("wolfterritories.org") != std::string::npos;
        return isWolfHost(gm->getGrid());
        // </WolfViewer 2026-09-27>
    }

    // <WolfViewer 2026-10-06> Logged in to Wolf Territories AND standing on one of its regions
    // now. Paul: "if the avatar tp's to another region in wolfstorm or wolfviewer go back to the
    // conventional map untill they get back to wolf territories grid". isWolfTerritories() is the
    // LOGIN grid and stays true after a hypergrid jump. The region says which grid it belongs to
    // in its SimulatorFeatures, OpenSimExtras "GridURL" (lfsimfeaturehandler.cpp:125 ->
    // hyperGridURL(), protocol stripped). OpenSim fills it from GatekeeperURI (Scene.cs:1288,
    // GridInfo.cs:407); every Wolf region has GatekeeperURI "http://grid.wolfterritories.org:8002".
    // Before the first region's features arrive the value is empty: then the login grid decides.
    //
    // <WolfViewer 2026-10-09> Paul: "other grids can't use these interfaces they are only for wolf".
    // Two holes closed. (1) A region that sends no GridURL left hyperGridURL() on the LOGIN grid's
    // gatekeeper (lfsimfeaturehandler.cpp fallback), so a foreign region counted as Wolf: once the
    // CURRENT region's features are in, only its own GridURL can make it Wolf. (2) While a new
    // region's features are on their way the previous region's answer stands - that keeps Flight
    // Mode, the dashboard and the game HUD steady across ordinary Wolf region crossings; anything
    // that SENDS to the region uses isOnWolfTerritoriesConfirmed() instead, which says no until
    // this region has said which grid it is.
    // <WolfViewer 2026-10-10> Paul, hypergridded to OSFest at 610 m: its sky thinned to space by the altitude sky, and Wolf's
    // automatic sky, waves, natural water, terrain paint and map overlays all kept running on a foreign grid - they tested
    // isWolfTerritories() (the LOGIN grid). Everything tied to the region underfoot (what is drawn, what is fetched or
    // changed for this region) uses this; isWolfTerritories() is only for the login itself (Wolf services, the toolbar).
    inline bool isOnWolfTerritories()
    {
        if (!isWolfTerritories())
        {
            return false;
        }
        if (!LFSimFeatureHandler::instanceExists())
        {
            return true;
        }
        const LFSimFeatureHandler& f = LFSimFeatureHandler::instance();
        const std::string region_grid = f.hyperGridURL();
        if (f.featuresRegionID().isNull())
        {
            return region_grid.empty() || isWolfHost(region_grid);   // nothing read yet: the login grid decides
        }
        return f.gridURLFromRegion() && isWolfHost(region_grid);
    }

    // On Wolf Territories AND the region the agent is in now has said so itself. For sending
    // anything Wolf-only to the region (game hello, wolfdrive), never for showing UI.
    inline bool isOnWolfTerritoriesConfirmed()
    {
        if (!isWolfTerritories() || !LFSimFeatureHandler::instanceExists())
        {
            return false;
        }
        const LLViewerRegion* region = gAgent.getRegion();
        const LFSimFeatureHandler& f = LFSimFeatureHandler::instance();
        return region && region->getRegionID() == f.featuresRegionID()
            && f.gridURLFromRegion() && isWolfHost(f.hyperGridURL());
    }
    // </WolfViewer 2026-10-09>
    // </WolfViewer 2026-10-06>

    // <WolfViewer 2026-10-08> Standing on a Wolf Territories region now, WHATEVER grid this viewer
    // logged in to: a hypergrid visitor too. Jimmy Olsen 10-08: Luke K came from DigiWorldz to his
    // sim in this viewer and saw fog and snow while the sim rained - isWolfTerritories() is the LOGIN
    // grid (DigiWorldz), so the region's weather was never read and his own Weather menu showed. The
    // region's grid address (SimulatorFeatures GridURL, as isOnWolfTerritories) says whose region it
    // is; empty (not arrived yet) = not known, so false.
    inline bool isOnWolfRegion()
    {
        if (!LFSimFeatureHandler::instanceExists())
        {
            return false;
        }
        const std::string region_grid = LFSimFeatureHandler::instance().hyperGridURL();
        return !region_grid.empty() && isWolfHost(region_grid);
    }

    // What the region's weather and sky follow: logged in here and on one of our regions, or a
    // visitor on one of our regions. Reading only - a visitor sends no session (see the callers).
    inline bool showsWolfRegionWeather()
    {
        return isOnWolfTerritories() || isOnWolfRegion();
    }
    // </WolfViewer 2026-10-08>

    // <WolfViewer 2026-09-27> Options for every request to a Wolf service (and the other fixed
    // third-party hosts this viewer calls). Those requests carry X-Wolf-Agent/X-Wolf-Session --
    // the live session id -- so the server must be proven to BE wolfstorm.app. HttpOptions
    // defaults verify the peer only when NoVerifySSLCert is off (httpoptions.cpp
    // sDefaultVerifyPeer, llappcorehttp.cpp:324) and never verify the host name
    // (httpoptions.cpp mVerifyHost(false)), so any valid certificate for ANY name would have
    // been accepted. Redirects are refused because curl forwards custom headers to the
    // redirect target. Same settings wolfai.cpp has always used. Every endpoint is https to a
    // host with a certificate that chains to ISRG Root X1 in the shipped ca-bundle.crt, and
    // none of them redirects (checked 2026-09-27).
    inline LLCore::HttpOptions::ptr_t makeVerifiedHttpOptions()
    {
        LLCore::HttpOptions::ptr_t opts = std::make_shared<LLCore::HttpOptions>();
        opts->setSSLVerifyPeer(true);
        opts->setSSLVerifyHost(true);
        opts->setFollowRedirects(false);
        return opts;
    }
    // </WolfViewer 2026-09-27>

    // The WolfStorm Rust proxy's HTTP API, on port 8080 of the host serving the viewer; the
    // primary host is wolfstorm.app (rust_proxy/src/main.rs:46-92 binds 8080 with TLS, and the
    // same box runs the whisper and piper engines).
    const char* const PROXY_API_BASE = "https://wolfstorm.app:8080";

    // Speech: POST /stt (wolfstorm js/ui/speech_to_text.js STT_URL) and POST /tts
    // (js/ui/text_to_speech.js TTS_URL).
    const char* const SPEECH_API_BASE = PROXY_API_BASE;

    // Model upload: POST /upload_mesh (wolfstorm js/ui/floaters/floater_mesh_upload.js PROXY;
    // handler rust_proxy/src/main.rs:1993 handle_upload_mesh). That endpoint writes the mesh
    // asset, the object asset and the inventory item straight into Wolf Territories' own ROBUST
    // services — GRID_ROBUST_BASE is hard-coded at main.rs:1638 — so it is meaningless, and
    // would be wrong, on any other grid. Every caller checks isWolfTerritories() first.
    const char* const MESH_API_BASE = PROXY_API_BASE;

    // The grid's Patreon, shown on the login screen. Same link WolfStorm uses
    // (wolfstorm/index.php:564).
    const char* const PATREON_URL = "https://www.patreon.com/15362110/join";

    // <WolfViewer 2026-10-06> What's new on the grid and in the viewers (gridmanager whatsnew.php,
    // from whatsnew.json). On the login screen and Help > What's new; the same page WolfStorm links.
    const char* const WHATSNEW_URL = "https://www.wolf-grid.com/index.php?f=whatsnew";

    // Screen sharing: the token endpoint (wolfstorm js/voice/screen_share.js TOKEN_URL, host
    // SHARE_HOST = 'wolfstorm.app') and the publisher page (wolfstorm/publish.php).
    const char* const SHARE_TOKEN_URL = "https://wolfstorm.app/php/screen_token.php";
    const char* const SHARE_PUBLISH_URL = "https://wolfstorm.app/publish.php";

    /**
     * Stop a floater being resized smaller than the panel it contains.
     *
     * [2026-09-11] WHY THIS EXISTS. floater_wolf_ai and floater_wolf_terrain_paint were split out
     * of the build floater and became resizable windows with min_height="320" — far below what
     * their contents need (414 and 404). Dragging either one smaller left the lower controls
     * PERFECTLY VISIBLE and completely dead to the mouse, because LLUI draws a child against the
     * SCREEN (llview.cpp:1310-1313 tests the ROOT's rect) but hit-tests it against its PARENT
     * (llview.cpp:799-806 `visibleEnabledAndContains`). A control below the shrunken panel's
     * bottom edge is therefore still painted and can never be clicked. It was reported as "the
     * Build button does nothing", and no amount of checking enabled state, commit callbacks or
     * overlapping widgets would ever have found it.
     *
     * Fixing it by editing min_height in the XML works exactly until the next layout change, so
     * the minimum is computed from the real contents instead: the bottom-most child decides it.
     * Only ever RAISES the limit, so a floater that is already generous keeps its own numbers.
     *
     * Not applied globally to LLFloater on purpose — a scrolling list is meant to shrink, and
     * forcing every floater to fit its contents would make some of them unusably large.
     */
    inline void fitFloaterToContents(LLPanel* panel, S32 margin = 16)
    {
        if (!panel) return;
        LLFloater* floater = panel->getParentByType<LLFloater>();
        if (!floater || !floater->isResizable()) return;

        S32 needed_w = 0;
        S32 needed_h = 0;
        for (const LLView* child : *panel->getChildList())
        {
            if (!child) continue;
            const LLRect& r = child->getRect();
            needed_w = llmax(needed_w, r.mRight);
            // Child rects are bottom-up; the panel's own height minus the child's bottom is how
            // far down the panel that child reaches.
            needed_h = llmax(needed_h, panel->getRect().getHeight() - r.mBottom);
        }
        if (needed_w <= 0 || needed_h <= 0) return;

        // The floater has to carry the panel plus its own chrome, so ask for the difference
        // between the two rather than assuming a header height.
        const S32 chrome_h = llmax(0, floater->getRect().getHeight() - panel->getRect().getHeight());
        const S32 chrome_w = llmax(0, floater->getRect().getWidth()  - panel->getRect().getWidth());

        const S32 min_w = needed_w + chrome_w + margin;
        const S32 min_h = needed_h + chrome_h + margin;

        LLRect cur = floater->getRect();
        floater->setResizeLimits(llmax(min_w, floater->getMinWidth()),
                                 llmax(min_h, floater->getMinHeight()));
        // A rect saved from before this guard existed can still be too small, so grow it now.
        if (cur.getWidth() < min_w || cur.getHeight() < min_h)
        {
            floater->reshape(llmax(cur.getWidth(), min_w), llmax(cur.getHeight(), min_h), true);
        }
    }
}

#endif // WOLF_GRID_H
