/**
 * @file wolfpassingplanes.cpp
 * @brief [PASSING PLANES 2026-11-01] Region/Estate > Region: "Passing planes over this region" (wolfpassingplanes.h).
 *
 * Who and why: an estate owner or manager of a themed region (a wild west town) wants the tour planes gone, or an airfield
 * owner wants them, in one click, and to see that it took. Flow: open the Region tab -> the state and its reason show ->
 * tick or untick -> "Saved" or a visible error that stays until the next action.
 *
 * Source of the request and reply: wolfstorm php/wolf_ambient.php "RESIDENT REQUESTS" (view / set). Source of the HTTP
 * pattern (verified options, agent + session headers, no retry on a save, json reply): wolfautoenvironment.cpp postCoro.
 * Source of the hidden chat: wolfnpc.cpp requestShips (send_chat_from_viewer, CHAT_TYPE_NORMAL, the RLV channel check).
 * Source of the "/74 traffic on|off" command: region-terrain-work/aircraft-dogfight-2026-11-01/c-wolf-air/src/AirTraffic.cs
 * Command() and AirDrones.cs CHANNEL = 74.
 * [SHIP OPT-IN 2026-11-03] "Passing ships on this region": the record's `boats` (wolf_ambient.php view/set what "boats",
 * wolf_ambient_lib.php wa_boats_block: off unless chosen, except Lone Wolf's regions). After a save the running wolf-naval
 * is nudged with "npcships {"op":"boats","req":0}" on channel 73 (wolf-naval 1.13.0 NpcFleetSites.cs Ui / UiBoats; channel
 * NpcRules.CHANNEL = 73, prefix NpcShipRules.UI_PREFIX, as wolfnpc.cpp SHIPS_CHANNEL / SHIPS_PREFIX): it reads the record
 * at once and answers in local chat. Paul 2026-11-03 "shorter rows": each switch is its box and one short line.
 * [SEA LIFE 2026-11-03] "Dolphins and whales on this region": the record's `sealife` (wolf_ambient.php view/set what
 * "sealife", wolf_ambient_lib.php wa_sealife_block: off unless chosen). No plugin to tell: wolfsealife.cpp draws them from
 * weather.php, so this viewer shows the choice at once (WolfRegionWeather::setSeaLifeLocal) and every other within a minute.
 * [UNDERSEA 2026-11-09] "Fish and sea plants on this region": the record's `undersea` (wolf_ambient.php view/set what
 * "undersea", wolf_ambient_lib.php wa_undersea_block: ON unless switched off). wolfundersea.cpp draws them from weather.php;
 * this viewer shows the choice at once (WolfRegionWeather::setUnderSeaLocal), every other within a minute.
 *
 * [STYLES 2026-11-10] t712 / t715 (Jimmy Olsen; Paul approved): "Kind of planes", "Kind of ships" and "Kind of birds", a
 * drop-down under each of those switches: the record's planes_style / boats_style / birds_zone (wolf_ambient.php view/set
 * what "planes_style" | "boats_style" | "birds_zone" with `style`; wolf_ambient_lib.php wa_style_block, wa_birds_zone_fields).
 * After a save: "/74 traffic style" (wolf-air 1.9.0 AirTraffic.Command reads the record again), the ships' own nudge
 * (wolf-naval 1.14.0 applies boats.style from the same get), the birds' zone shown here at once
 * (WolfRegionWeather::setBirdsZoneLocal). Design: region-terrain-work/traffic-birds-styles-2026-11-10/README.md.
 *
 * Copyright (C) 2026 IntelligentWolf Ltd.
 */

#include "llviewerprecompiledheaders.h"

#include "wolfpassingplanes.h"

#include <boost/json.hpp>

#include "llagent.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"   // [STYLES 2026-11-10/]
#include "llchat.h"
#include "llcorehttputil.h"
#include "llfloater.h"   // [TICKET 481 2026-11-03/] layout() grows the window; [TICKET 529 2026-11-05/] gFloaterView
#include "llcoros.h"
#include "lleventcoro.h"
#include "llframetimer.h"
#include "llhttpconstants.h"
#include "llnotificationsutil.h"
#include "llpanel.h"
#include "llscrollcontainer.h"   // [TICKET 529 2026-11-05/] the Region panel sits in one
#include "llui.h"                // [TICKET 529 2026-11-05/] LLUICachedControl (UIScrollbarSize)
#include "llsdjson.h"
#include "lltextbox.h"
#include "llviewerregion.h"
#include "rlvactions.h"
#include "rlvhandler.h"
#include "wolfgrid.h"
#include "wolfregionweather.h"   // [BIRDS 2026-11-02/] setBirdsLocal; [NATURE SOUNDS 2026-11-03/] setSoundsLocal; [SEA LIFE 2026-11-03/] setSeaLifeLocal

// Source: fsnearbychathub.cpp:107 (declared as wolfnpc.cpp:201 declares it) - the viewer's own chat path, so RLV's channel
// rules apply; nothing is added to the resident's chat history
void send_chat_from_viewer(std::string utf8_out_text, EChatType type, S32 channel);

namespace
{
    // Source: wolfautoenvironment.cpp:47 API_URL - the same host; php/wolf_ambient.php is its neighbour.
    const char* const API_URL = "https://wolfstorm.app/php/wolf_ambient.php";
    // Source: c-wolf-air/src/AirDrones.cs:29 CHANNEL = 74; AirPlugin.cs OnEvent hands channel 74 chat to AirTraffic.Command.
    const S32 AIR_CHANNEL = 74;
    // [SHIP OPT-IN 2026-11-03] Source: wolfnpc.cpp:64-65 SHIPS_CHANNEL = 73, SHIPS_PREFIX "npcships " (wolf-naval NpcRules.CHANNEL,
    // NpcShipRules.UI_PREFIX); the op is NpcFleetSites.cs Ui "boats"
    const S32 SHIPS_CHANNEL = 73;
    const char* const SHIPS_NUDGE = "npcships {\"op\":\"boats\",\"req\":0}";
    // Read once per window open / region change (review 2026-11-01: no polling while the window stays open). Several
    // refreshFromRegion calls in one open count as one; a failed read may be tried again after RETRY_SEC.
    const F64 REREAD_DEBOUNCE_SEC = 2.0;
    const F64 RETRY_SEC = 10.0;
    // How long a read waits for the region to confirm it is on Wolf Territories (the session is only sent then).
    const F32 CONFIRM_WAIT_SEC = 15.f;

    struct State
    {
        LLUUID mRegion;            // the region the state below belongs to
        bool mHave = false;        // a "view" reply is in mReply
        bool mBusy = false;        // a read or a save is on its way
        LLSD mReply;               // {success, ambient (undefined = nobody has chosen), default, on, cats, mayChange}
        std::string mStatus;       // the status line
        bool mStatusError = false;
        bool mStatusFromRead = false;  // the status line is the read's own ("Reading..." or a read failure)
        F64 mFetchedAt = -1.e9;
        F64 mFailedAt = 0.0;       // when the last read failed (0: it did not)
        U64 mSerial = 0;           // bumped on every region change, so an old reply is dropped
    };
    State sState;

    LLSD json_to_llsd(const LLSD::Binary& bytes)
    {
        // Source: wolfautoenvironment.cpp:68 json_to_llsd
        std::string text(bytes.begin(), bytes.end());
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(text, ec);
        if (ec) return LLSD();
        return LlsdFromJson(v);
    }

    /** [NATURE SOUNDS 2026-11-03] Which switch a save is for (wolf_ambient.php set {what}). */
    enum What { WHAT_PLANES, WHAT_BIRDS, WHAT_SOUNDS, WHAT_SHIPS, WHAT_SEALIFE, WHAT_UNDERSEA };   // not PLANES: wingdi.h #defines PLANES (GetDeviceCaps index 14)

    /** [STYLES 2026-11-10] Which kind a drop-down saves (wolf_ambient.php set {what}). */
    enum Kind { KIND_PLANES, KIND_SHIPS, KIND_BIRDS };
    const char* kind_what(Kind k) { return k == KIND_PLANES ? "planes_style" : k == KIND_SHIPS ? "boats_style" : "birds_zone"; }
    const char* kind_combo(Kind k) { return k == KIND_PLANES ? "wolf_planes_kind" : k == KIND_SHIPS ? "wolf_ships_kind" : "wolf_birds_kind"; }

    void set_status(const std::string& msg, bool error, bool from_read = false)
    {
        sState.mStatus = msg;
        sState.mStatusError = error;
        sState.mStatusFromRead = from_read;
    }

    /** [TICKET 529 2026-11-05] How far layout() has grown the Region / Estate window below its own height, so it can give
     *  that back (the texts got shorter, the Wolf block went, the viewer window got smaller) and never grows past it. */
    struct Growth
    {
        LLHandle<LLFloater> mFloater;
        S32 mBy = 0;
    };
    Growth sGrowth;

    /** [TICKET 481 2026-11-03] Buzzy Cnayl: the planes' and birds' texts overlapped (fixed-height boxes, and the wrapped text
     *  ran past them). Stacks the Wolf block top-down from the planes box (panel_region_general.xml): each text box as tall
     *  as its wrapped text (LLTextBase::getTextBoundingRect reflows it at the box's width), at any font size. Every text is
     *  as wide as the panel less the XML's 10 px right margin (the panel is 480 wide there and the texts end at 470), so
     *  none runs under the scroll bar. Returns the stack's bottom edge
     *  (panel coordinates: y grows upwards, 0 is the panel's bottom). */
    S32 stack(LLPanel* panel, LLView* anchor)
    {
        constexpr S32 AFTER_CHECK = 1, AFTER_TEXT = 5, RIGHT_MARGIN = 10, MIN_TEXT = 14;
        const S32 right = panel->getRect().getWidth() - RIGHT_MARGIN;
        S32 top = anchor->getRect().mBottom - AFTER_CHECK;
        auto place = [&top](LLView* v, S32 height, S32 gap)
        {
            LLRect r = v->getRect();
            r.setLeftTopAndSize(r.mLeft, top, r.getWidth(), height);
            v->setShape(r);
            top = r.mBottom - gap;
        };
        auto text = [&](const char* name, S32 gap)
        {
            LLTextBox* t = panel->findChild<LLTextBox>(name);
            if (!t) return;
            const bool empty = t->getText().empty();
            // the width first: the wrapped height is measured at the width the box will have
            LLRect r = t->getRect();
            r.mRight = r.mLeft + llmax(0, right - r.mLeft);
            t->setShape(r);
            place(t, empty ? 0 : llmax(MIN_TEXT, t->getTextPixelHeight() + 2), empty ? 0 : gap);
        };
        auto check = [&](const char* name)
        {
            if (LLView* c = panel->findChildView(name)) place(c, c->getRect().getHeight(), AFTER_CHECK);
        };
        // [STYLES 2026-11-10] a kind row: its label and its drop-down side by side, as tall as the drop-down
        auto kind = [&](const char* label, const char* combo)
        {
            LLView* c = panel->findChildView(combo);
            if (!c) return;
            const S32 h = c->getRect().getHeight();
            if (LLView* l = panel->findChildView(label))
            {
                LLRect r = l->getRect();
                r.setLeftTopAndSize(r.mLeft, top, r.getWidth(), h);
                l->setShape(r);
            }
            place(c, h, AFTER_TEXT);
        };
        text("wolf_planes_state", AFTER_TEXT);
        kind("wolf_planes_kind_label", "wolf_planes_kind");   // [STYLES 2026-11-10/]
        check("wolf_ships_check");                // [SHIP OPT-IN 2026-11-03/]
        text("wolf_ships_state", AFTER_TEXT);
        kind("wolf_ships_kind_label", "wolf_ships_kind");     // [STYLES 2026-11-10/]
        check("wolf_birds_check");
        text("wolf_birds_state", AFTER_TEXT);
        kind("wolf_birds_kind_label", "wolf_birds_kind");     // [STYLES 2026-11-10/]
        text("wolf_birds_kind_state", AFTER_TEXT);
        check("wolf_sounds_check");
        text("wolf_sounds_state", AFTER_TEXT);
        check("wolf_sealife_check");              // [SEA LIFE 2026-11-03/]
        text("wolf_sealife_state", AFTER_TEXT);
        check("wolf_undersea_check");             // [UNDERSEA 2026-11-09/]
        text("wolf_undersea_state", AFTER_TEXT);
        text("wolf_planes_status", 0);
        return top;
    }

    /** [TICKET 481 2026-11-03] stacks the Wolf block (stack above) each time its texts change.
     *  [TICKET 529 2026-11-05] Jimmy Olsen, 1360x768: 481 then grew the window by whatever the stack was short, so with five
     *  switches (W94's dolphins and whales) the window ran under the bottom toolbars and the last switches could not be
     *  reached. The panel now sits in a scroll container (llfloaterregioninfo.cpp postBuild): it is made as tall as its
     *  contents (never less than the view), the window grows only while its bottom stays inside the floater view's snap
     *  rect (LLFloaterView::getSnapRect: the space between the toolbars, where floaters are kept), and the rest scrolls.
     *  When the scroll bar shows, the panel is narrowed by its width and the block restacked at that width. */
    void layout(LLPanel* panel)
    {
        LLView* anchor = panel->findChildView("wolf_planes_check");
        const bool wolf = anchor && anchor->getVisible();
        LLScrollContainer* scroll = dynamic_cast<LLScrollContainer*>(panel->getParent());
        if (!scroll)
        {
            if (wolf) stack(panel, anchor);
            return;
        }
        LLFloater* floater = panel->getParentByType<LLFloater>();
        if (!floater || floater->isMinimized()) return;   // a minimised window's panel is squeezed: leave it until restored
        constexpr S32 BOTTOM_MARGIN = 8;
        const S32 border = scroll->getBorderWidth();
        auto view_width = [scroll, border]() { return scroll->getRect().getWidth() - 2 * border; };
        auto view_height = [scroll, border]() { return scroll->getRect().getHeight() - 2 * border; };
        // How tall the panel must be to hold the stack: the stack's bottom is `bottom` above the panel's bottom now.
        auto fit = [panel, anchor, wolf](S32 width) -> S32
        {
            LLRect r = panel->getRect();
            if (r.getWidth() != width)
            {
                r.mRight = r.mLeft + width;
                panel->setShape(r);   // the right-hand controls follow the right edge
            }
            if (!wolf) return 0;
            const S32 bottom = stack(panel, anchor);
            return panel->getRect().getHeight() - bottom + BOTTOM_MARGIN;
        };
        S32 needed = fit(view_width());

        if (sGrowth.mFloater.get() != floater)
        {
            sGrowth.mFloater = floater->getHandle();
            sGrowth.mBy = 0;
        }
        LLRect fr = floater->getRect();
        // How far the bottom may still go down: to the snap rect's bottom, in the floater view's coordinates (the floater's
        // parent). A floater anywhere else may only give back what it was given.
        S32 room = 0;
        if (gFloaterView && floater->getParent() == gFloaterView)
        {
            room = fr.mBottom - gFloaterView->getSnapRect().mBottom;
        }
        const S32 base_height = view_height() - sGrowth.mBy;   // the view at the window's own height
        const S32 grow = llclamp(needed - base_height, 0, llmax(0, sGrowth.mBy + room));
        if (grow != sGrowth.mBy)
        {
            fr.mBottom -= grow - sGrowth.mBy;   // the top stays where it is
            floater->setShape(fr);
            sGrowth.mBy = grow;
        }

        if (needed > view_height())
        {
            static LLUICachedControl<S32> scrollbar_size_control("UIScrollbarSize", 0);   // as llscrollcontainer.cpp
            const S32 scrollbar = scroll->getSize() == -1 ? (S32)scrollbar_size_control : scroll->getSize();
            needed = fit(view_width() - scrollbar);
        }
        LLRect r = panel->getRect();
        r.setLeftTopAndSize(r.mLeft, r.mTop, r.getWidth(), llmax(view_height(), needed));
        panel->setShape(r);   // every control follows the top; the scroll container puts the panel back in view as it draws
    }

    std::string str(LLPanel* panel, const char* name)
    {
        return panel->getString(name);
    }

    /** The state line: on or off, and whether that is the default (and why) or someone's choice. */
    std::string state_line(LLPanel* panel)
    {
        if (!sState.mHave) return std::string();
        const LLSD& r = sState.mReply;
        const bool chosen = r["ambient"].isBoolean();
        const bool on = r["on"].asBoolean();
        if (chosen) return str(panel, on ? "on_chosen" : "off_chosen");
        return str(panel, on ? "planes_on_default" : "off_unless");
    }

    /** [BIRDS 2026-11-02] The same for "Birds over this region": the reply's `birds` block (wolf_ambient.php view/set),
     *  absent from an older grid, which leaves the line empty and the box unticked and greyed. */
    bool have_birds()
    {
        return sState.mHave && sState.mReply["birds"].isMap() && sState.mReply["birds"]["on"].isBoolean();
    }

    std::string birds_line(LLPanel* panel)
    {
        if (!have_birds()) return std::string();
        const LLSD& b = sState.mReply["birds"];
        const bool on = b["on"].asBoolean();
        if (b["ambient"].isBoolean()) return str(panel, on ? "on_chosen" : "off_chosen");
        // [BIRDS 2026-11-02] wolf_ambient_lib.php wa_birds_default: Parks / Marinas on, other categories off, none = sea
        if (!b["default"].asBoolean() && b["seaCoast"].asBoolean()) return str(panel, "birds_sea_default");
        return str(panel, on ? "birds_on_default" : "off_unless");
    }

    /** [NATURE SOUNDS 2026-11-03] "Nature sounds on this region": the reply's `sounds` block {ambient, default (false), on}
     *  (wolf_ambient.php view/set, wolf_ambient_lib.php wa_sounds_block); absent from an older grid: line empty, box greyed. */
    bool have_sounds()
    {
        return sState.mHave && sState.mReply["sounds"].isMap() && sState.mReply["sounds"]["on"].isBoolean();
    }

    std::string sounds_line(LLPanel* panel)
    {
        if (!have_sounds()) return std::string();
        const LLSD& b = sState.mReply["sounds"];
        const bool on = b["on"].asBoolean();
        if (b["ambient"].isBoolean()) return str(panel, on ? "on_chosen" : "off_chosen");
        return str(panel, "off_unless");   // nobody has chosen: off (there is no other default)
    }

    /** [SHIP OPT-IN 2026-11-03] "Passing ships on this region": the reply's `boats` block {ambient, default (Lone Wolf's
     *  region), on} (wolf_ambient.php view/set, wolf_ambient_lib.php wa_boats_block); absent from an older grid: greyed. */
    bool have_ships()
    {
        return sState.mHave && sState.mReply["boats"].isMap() && sState.mReply["boats"]["on"].isBoolean();
    }

    std::string ships_line(LLPanel* panel)
    {
        if (!have_ships()) return std::string();
        const LLSD& b = sState.mReply["boats"];
        const bool on = b["on"].asBoolean();
        if (b["ambient"].isBoolean()) return str(panel, on ? "on_chosen" : "off_chosen");
        return str(panel, on ? "ships_on_default" : "off_unless");
    }

    /** [SEA LIFE 2026-11-03] "Dolphins and whales on this region": the reply's `sealife` block {ambient, default (false),
     *  on} (wolf_ambient.php view/set, wolf_ambient_lib.php wa_sealife_block); absent from an older grid: line empty, greyed. */
    bool have_sealife()
    {
        return sState.mHave && sState.mReply["sealife"].isMap() && sState.mReply["sealife"]["on"].isBoolean();
    }

    std::string sealife_line(LLPanel* panel)
    {
        if (!have_sealife()) return std::string();
        const LLSD& b = sState.mReply["sealife"];
        const bool on = b["on"].asBoolean();
        if (b["ambient"].isBoolean()) return str(panel, on ? "on_chosen" : "off_chosen");
        return str(panel, "off_unless");   // nobody has chosen: off (there is no other default)
    }

    /** [UNDERSEA 2026-11-09] "Fish and sea plants on this region": the reply's `undersea` block {ambient, default (true),
     *  on} (wolf_ambient.php view/set, wolf_ambient_lib.php wa_undersea_block); absent from an older grid: line empty, greyed. */
    bool have_undersea()
    {
        return sState.mHave && sState.mReply["undersea"].isMap() && sState.mReply["undersea"]["on"].isBoolean();
    }

    std::string undersea_line(LLPanel* panel)
    {
        if (!have_undersea()) return std::string();
        const LLSD& b = sState.mReply["undersea"];
        const bool on = b["on"].asBoolean();
        if (b["ambient"].isBoolean()) return str(panel, on ? "on_chosen" : "off_chosen");
        return str(panel, on ? "undersea_on_default" : "off_unless");
    }

    /** [STYLES 2026-11-10] The reply carries this kind (an older grid does not: the drop-down stays greyed). */
    bool have_kind(Kind k)
    {
        if (!sState.mHave) return false;
        const LLSD& r = sState.mReply;
        if (k == KIND_PLANES) return r["planesStyle"].isMap() && r["planesStyle"]["style"].isString();
        if (k == KIND_SHIPS) return r["boatsStyle"].isMap() && r["boatsStyle"]["style"].isString();
        return have_birds() && r["birds"]["zone"].isString();
    }

    /** [STYLES 2026-11-10] The drop-down's value: the owner's choice, or "auto". */
    std::string kind_value(Kind k)
    {
        const LLSD& r = sState.mReply;
        const LLSD chosen = k == KIND_PLANES ? r["planesStyle"]["chosen"] : k == KIND_SHIPS ? r["boatsStyle"]["chosen"] : r["birds"]["zoneChosen"];
        return chosen.isString() ? chosen.asString() : std::string("auto");
    }

    /** [STYLES 2026-11-10] What the region has now (Automatic worked out by the grid). */
    std::string kind_resolved(Kind k)
    {
        const LLSD& r = sState.mReply;
        return (k == KIND_PLANES ? r["planesStyle"]["style"] : k == KIND_SHIPS ? r["boatsStyle"]["style"] : r["birds"]["zone"]).asString();
    }

    /** [STYLES 2026-11-10] A kind in the window's words (the panel's kind_* strings). */
    std::string kind_words(LLPanel* panel, Kind k, const std::string& id)
    {
        if (id == "modern") return str(panel, k == KIND_PLANES ? "kind_modern_planes" : "kind_modern_ships");
        static const char* const KNOWN[] = { "auto", "vintage", "scifi", "sail", "eastern", "tropical", "subtropical", "temperate", "polar" };
        for (const char* known : KNOWN)
            if (id == known) return str(panel, (std::string("kind_") + known).c_str());
        return id;
    }

    std::string with_kind(std::string text, const std::string& kind)
    {
        const size_t at = text.find("[KIND]");
        if (at != std::string::npos) text.replace(at, 6, kind);
        return text;
    }

    /** [STYLES 2026-11-10] The line under the birds' drop-down: the owner's choice, or what Automatic gives here and why. */
    std::string birds_kind_line(LLPanel* panel)
    {
        if (!have_kind(KIND_BIRDS)) return std::string();
        const LLSD& b = sState.mReply["birds"];
        const std::string words = kind_words(panel, KIND_BIRDS, b["zone"].asString());
        if (b["zoneChosen"].isString()) return with_kind(str(panel, "kind_birds_chosen"), words);
        if (!b["lat"].isReal() && !b["lat"].isInteger()) return with_kind(str(panel, "kind_birds_auto_noplace"), words);
        const F64 lat = b["lat"].asReal();
        const std::string at = llformat("%d\xC2\xB0 %s", (S32)ll_round(fabs(lat)), lat < 0.0 ? "south" : "north");   // degree sign, UTF-8
        std::string line = with_kind(str(panel, "kind_birds_auto"), words);
        const size_t p = line.find("[LAT]");
        if (p != std::string::npos) line.replace(p, 5, at);
        return line;
    }

    void write_controls(LLPanel* panel)
    {
        LLCheckBoxCtrl* check = panel->findChild<LLCheckBoxCtrl>("wolf_planes_check");
        LLTextBox* state = panel->findChild<LLTextBox>("wolf_planes_state");
        LLTextBox* status = panel->findChild<LLTextBox>("wolf_planes_status");
        if (!check || !state || !status) return;
        const bool may = sState.mHave && sState.mReply["mayChange"].asBoolean();
        check->set(sState.mHave && sState.mReply["on"].asBoolean());
        check->setEnabled(may && !sState.mBusy);
        // [SHIP OPT-IN 2026-11-03] the ships box and its line, from the same reply
        if (LLCheckBoxCtrl* ships = panel->findChild<LLCheckBoxCtrl>("wolf_ships_check"))
        {
            ships->set(have_ships() && sState.mReply["boats"]["on"].asBoolean());
            ships->setEnabled(may && !sState.mBusy && have_ships());
        }
        if (LLTextBox* ships_state = panel->findChild<LLTextBox>("wolf_ships_state"))
        {
            ships_state->setEnabled(true);
            ships_state->setText(ships_line(panel));
        }
        // [BIRDS 2026-11-02] the birds box and its line, from the same reply
        if (LLCheckBoxCtrl* birds = panel->findChild<LLCheckBoxCtrl>("wolf_birds_check"))
        {
            birds->set(have_birds() && sState.mReply["birds"]["on"].asBoolean());
            birds->setEnabled(may && !sState.mBusy && have_birds());
        }
        if (LLTextBox* birds_state = panel->findChild<LLTextBox>("wolf_birds_state"))
        {
            birds_state->setEnabled(true);
            birds_state->setText(birds_line(panel));
        }
        // [NATURE SOUNDS 2026-11-03] the nature sounds box, its line and its own status line, from the same reply
        if (LLCheckBoxCtrl* sounds = panel->findChild<LLCheckBoxCtrl>("wolf_sounds_check"))
        {
            sounds->set(have_sounds() && sState.mReply["sounds"]["on"].asBoolean());
            sounds->setEnabled(may && !sState.mBusy && have_sounds());
        }
        if (LLTextBox* sounds_state = panel->findChild<LLTextBox>("wolf_sounds_state"))
        {
            sounds_state->setEnabled(true);
            sounds_state->setText(sounds_line(panel));
        }
        // [SEA LIFE 2026-11-03] the dolphins and whales box and its line, from the same reply
        if (LLCheckBoxCtrl* sealife = panel->findChild<LLCheckBoxCtrl>("wolf_sealife_check"))
        {
            sealife->set(have_sealife() && sState.mReply["sealife"]["on"].asBoolean());
            sealife->setEnabled(may && !sState.mBusy && have_sealife());
        }
        if (LLTextBox* sealife_state = panel->findChild<LLTextBox>("wolf_sealife_state"))
        {
            sealife_state->setEnabled(true);
            sealife_state->setText(sealife_line(panel));
        }
        // [UNDERSEA 2026-11-09] the fish and sea plants box and its line, from the same reply
        if (LLCheckBoxCtrl* undersea = panel->findChild<LLCheckBoxCtrl>("wolf_undersea_check"))
        {
            undersea->set(have_undersea() && sState.mReply["undersea"]["on"].asBoolean());
            undersea->setEnabled(may && !sState.mBusy && have_undersea());
        }
        if (LLTextBox* undersea_state = panel->findChild<LLTextBox>("wolf_undersea_state"))
        {
            undersea_state->setEnabled(true);
            undersea_state->setText(undersea_line(panel));
        }
        // [STYLES 2026-11-10] the three kinds, from the same reply; the labels stay readable for everyone
        for (Kind k : { KIND_PLANES, KIND_SHIPS, KIND_BIRDS })
        {
            if (LLComboBox* combo = panel->findChild<LLComboBox>(kind_combo(k)))
            {
                if (have_kind(k)) combo->setValue(LLSD(kind_value(k)));
                combo->setEnabled(may && !sState.mBusy && have_kind(k));
            }
        }
        for (const char* label : { "wolf_planes_kind_label", "wolf_ships_kind_label", "wolf_birds_kind_label" })
        {
            if (LLView* l = panel->findChildView(label)) l->setEnabled(true);
        }
        if (LLTextBox* birds_kind_state = panel->findChild<LLTextBox>("wolf_birds_kind_state"))
        {
            birds_kind_state->setEnabled(true);
            birds_kind_state->setText(birds_kind_line(panel));
        }
        // setCtrlsEnabled greys every child for someone who may not manage the estate; these lines must stay readable.
        state->setEnabled(true);
        status->setEnabled(true);
        state->setText(state_line(panel));
        // Same colours as the Automatic Environment tab (wolfautoenvironment.cpp:185 setStatus): a failure must be readable.
        const LLColor4 colour = sState.mStatusError ? LLColor4(0.88f, 0.33f, 0.28f, 1.f) : LLColor4(0.79f, 0.64f, 0.15f, 1.f);
        const std::string not_yours = (sState.mHave && !may) ? str(panel, "planes_not_yours") : std::string();
        std::string msg = sState.mStatus;
        if (msg.empty()) msg = not_yours;
        status->setText(msg);
        status->setColor(colour);
        layout(panel);   // [TICKET 481 2026-11-03/] the texts may have changed height
    }

    LLSD post_and_wait(const std::string& body, bool save, LLCore::HttpStatus& status)
    {
        // Source: wolfautoenvironment.cpp:608-631 postCoro (verified options, the agent + session headers, a JSON body).
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfPassingPlanes", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
        options->setTimeout(30);
        if (save) options->setRetries(0);   // a save is never sent twice by the HTTP layer (wolfautoenvironment.cpp:615)
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "application/json");
        headers->append("X-Wolf-Agent", gAgentID.asString());
        headers->append("X-Wolf-Session", gAgentSessionID.asString());
        LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
        raw->append(body.data(), body.size());
        LLSD result = adapter->postRawAndSuspend(request, API_URL, raw, options, headers);
        status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
        LLSD reply;
        if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
            reply = json_to_llsd(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary());
        return reply;
    }

    std::string why_failed(const LLCore::HttpStatus& status, const LLSD& reply)
    {
        // Source: wolfautoenvironment.cpp:633-634 - the grid's own words when it gave some
        if (reply.isMap() && reply.has("error")) return reply["error"].asString();
        if (!status) return status.toString();
        return "the service returned an unreadable reply";
    }

    /** [STYLES 2026-11-10] wolf_ambient.php set {region, what: planes_style | boats_style | birds_zone, style}. */
    std::string kind_body(const LLUUID& region, Kind k, const std::string& style)
    {
        boost::json::object o;
        o["action"] = "set";
        o["region"] = region.asString();
        o["what"] = kind_what(k);
        o["style"] = style;
        return boost::json::serialize(o);
    }

    std::string request_body(const char* action, const LLUUID& region, int ambient, What what = WHAT_PLANES)
    {
        boost::json::object o;
        o["action"] = action;
        o["region"] = region.asString();
        if (what == WHAT_BIRDS) o["what"] = "birds";    // [BIRDS 2026-11-02/] wolf_ambient.php set {what: "birds"}
        if (what == WHAT_SOUNDS) o["what"] = "sounds";  // [NATURE SOUNDS 2026-11-03/] wolf_ambient.php set {what: "sounds"}
        if (what == WHAT_SHIPS) o["what"] = "boats";    // [SHIP OPT-IN 2026-11-03/] wolf_ambient.php set {what: "boats"}
        if (what == WHAT_SEALIFE) o["what"] = "sealife";   // [SEA LIFE 2026-11-03/] wolf_ambient.php set {what: "sealife"}
        if (what == WHAT_UNDERSEA) o["what"] = "undersea"; // [UNDERSEA 2026-11-09/] wolf_ambient.php set {what: "undersea"}
        if (ambient >= 0) o["ambient"] = ambient == 1;
        return boost::json::serialize(o);
    }

    void fetch_coro(LLHandle<LLPanel> handle, LLUUID region, U64 serial)
    {
        // The session goes only to Wolf Territories: wait (briefly) for this region to confirm it.
        LLFrameTimer waited;
        while (!WolfGrid::isOnWolfTerritoriesConfirmed() && waited.getElapsedTimeF32() < CONFIRM_WAIT_SEC)
        {
            llcoro::suspendUntilTimeout(0.5f);
            if (sState.mSerial != serial) return;
        }
        LLPanel* panel = handle.get();
        if (sState.mSerial != serial) return;
        if (!WolfGrid::isOnWolfTerritoriesConfirmed() || !gAgent.getRegion() || gAgent.getRegion()->getRegionID() != region)
        {
            sState.mBusy = false;
            sState.mFailedAt = LLFrameTimer::getTotalSeconds();
            if (!panel) return;
            set_status(str(panel, "planes_read_failed") + " " + str(panel, "planes_not_wolf"), true, true);
            write_controls(panel);
            return;
        }
        LLCore::HttpStatus status;
        const LLSD reply = post_and_wait(request_body("view", region, -1), false, status);
        if (sState.mSerial != serial) return;   // moved on to another region meanwhile
        sState.mBusy = false;
        panel = handle.get();
        if (!status || !reply.isMap() || !reply["success"].asBoolean())
        {
            sState.mFailedAt = LLFrameTimer::getTotalSeconds();
            if (!panel) return;
            set_status(str(panel, "planes_read_failed") + " " + why_failed(status, reply), true, true);
            write_controls(panel);
            return;
        }
        sState.mReply = reply;
        sState.mHave = true;
        sState.mFailedAt = 0.0;
        // a save's message (or failure) stays until the next action; only the read's own line goes
        if (sState.mStatusFromRead) set_status(std::string(), false);
        if (panel) write_controls(panel);
    }

    void save_coro(LLHandle<LLPanel> handle, LLUUID region, bool on, What what, U64 serial)
    {
        const bool birds = (what == WHAT_BIRDS);
        const bool sounds = (what == WHAT_SOUNDS);   // [NATURE SOUNDS 2026-11-03/]
        const bool ships = (what == WHAT_SHIPS);     // [SHIP OPT-IN 2026-11-03/]
        const bool sealife = (what == WHAT_SEALIFE); // [SEA LIFE 2026-11-03/]
        const bool undersea = (what == WHAT_UNDERSEA);   // [UNDERSEA 2026-11-09/]
        LLCore::HttpStatus status;
        const LLSD reply = post_and_wait(request_body("set", region, on ? 1 : 0, what), true, status);
        const bool ok = status && reply.isMap() && reply["success"].asBoolean();
        LLPanel* panel = handle.get();
        if (!ok)
        {
            // A change the owner asked for must not fail silently, even with the window closed (wolfautoenvironment.cpp:640).
            // [BIRDS 2026-11-02] Always, not only with the window closed: the status line under the two boxes is 44 high and
            // a long refusal would be cut off there; the alert keeps the whole text, to read and copy.
            LLNotificationsUtil::add("GenericAlertOK", LLSD().with("MESSAGE",
                std::string(birds ? "Could not save birds: " : sounds ? "Could not save nature sounds: "
                            : ships ? "Could not save passing ships: " : sealife ? "Could not save dolphins and whales: "
                            : undersea ? "Could not save fish and sea plants: "
                            : "Could not save passing planes: ")
                + why_failed(status, reply)));
        }
        if (sState.mSerial != serial) return;
        sState.mBusy = false;
        if (!ok)
        {
            if (panel)
            {
                set_status(str(panel, birds ? "birds_save_failed" : sounds ? "sounds_save_failed" : ships ? "ships_save_failed"
                               : sealife ? "sealife_save_failed" : undersea ? "undersea_save_failed" : "planes_save_failed") + " "
                           + why_failed(status, reply), true);
                write_controls(panel);   // the check box goes back to what the grid has
            }
            return;
        }
        sState.mReply = reply;
        sState.mHave = true;
        if (ships)
        {
            // [SHIP OPT-IN 2026-11-03] Nudge the running wolf-naval on the region the change was for: it reads the record at
            // once (ships at sea sail away, or come), and says "Passing ships on this region: on|off." in local chat.
            const bool saved_on = have_ships() && sState.mReply["boats"]["on"].asBoolean();
            bool told = false;
            if (WolfGrid::isOnWolfTerritoriesConfirmed() && gAgent.getRegion() && gAgent.getRegion()->getRegionID() == region
                && !(rlv_handler_t::isEnabled() && !RlvActions::canSendChannel(SHIPS_CHANNEL)))
            {
                send_chat_from_viewer(SHIPS_NUDGE, CHAT_TYPE_NORMAL, SHIPS_CHANNEL);
                told = true;
            }
            if (panel)
            {
                std::string msg = str(panel, saved_on ? "ships_saved_on" : "ships_saved_off");
                if (!told) msg += " " + str(panel, "ships_later");
                set_status(msg, false);
                write_controls(panel);
            }
            return;
        }
        if (undersea)
        {
            // [UNDERSEA 2026-11-09] No plugin to tell: the viewers draw the plants and fish from weather.php, which every
            // viewer here reads within the minute. This viewer shows the choice now.
            const bool saved_on = have_undersea() && sState.mReply["undersea"]["on"].asBoolean();
            if (WolfRegionWeather::instanceExists()) WolfRegionWeather::instance().setUnderSeaLocal(region, saved_on);
            if (panel)
            {
                set_status(str(panel, saved_on ? "undersea_saved_on" : "undersea_saved_off"), false);
                write_controls(panel);
            }
            return;
        }
        if (sealife)
        {
            // [SEA LIFE 2026-11-03] No plugin to tell: the viewers draw the dolphins and whales from weather.php, which every
            // viewer here reads within the minute. This viewer shows the choice now.
            const bool saved_on = have_sealife() && sState.mReply["sealife"]["on"].asBoolean();
            if (WolfRegionWeather::instanceExists()) WolfRegionWeather::instance().setSeaLifeLocal(region, saved_on);
            if (panel)
            {
                set_status(str(panel, saved_on ? "sealife_saved_on" : "sealife_saved_off"), false);
                write_controls(panel);
            }
            return;
        }
        if (sounds)
        {
            // [NATURE SOUNDS 2026-11-03] No plugin to tell: every viewer here reads it from weather.php within the minute.
            // This viewer plays (or stops) now; the climate for the insects comes with the next weather read.
            const bool saved_on = have_sounds() && sState.mReply["sounds"]["on"].asBoolean();
            if (WolfRegionWeather::instanceExists()) WolfRegionWeather::instance().setSoundsLocal(region, saved_on);
            if (panel)
            {
                set_status(str(panel, saved_on ? "sounds_saved_on" : "sounds_saved_off"), false);
                write_controls(panel);
            }
            return;
        }
        if (birds)
        {
            // [BIRDS 2026-11-02] No plugin to tell: the viewers draw the birds from weather.php, which every viewer here
            // reads within the minute. This viewer shows the choice now.
            const bool saved_on = have_birds() && sState.mReply["birds"]["on"].asBoolean();
            if (WolfRegionWeather::instanceExists()) WolfRegionWeather::instance().setBirdsLocal(region, saved_on);
            if (panel)
            {
                set_status(str(panel, saved_on ? "birds_saved_on" : "birds_saved_off"), false);
                write_controls(panel);
            }
            return;
        }
        // Tell the running wolf-air now (it reads the record only every 30 minutes): its own "/74 traffic on|off", which
        // checks the same estate rule and answers with a short alert. Only on the region the change was for.
        bool told = false;
        if (WolfGrid::isOnWolfTerritoriesConfirmed() && gAgent.getRegion() && gAgent.getRegion()->getRegionID() == region
            && !(rlv_handler_t::isEnabled() && !RlvActions::canSendChannel(AIR_CHANNEL)))
        {
            send_chat_from_viewer(on ? "traffic on" : "traffic off", CHAT_TYPE_NORMAL, AIR_CHANNEL);
            told = true;
        }
        if (panel)
        {
            std::string msg = str(panel, on ? "planes_saved_on" : "planes_saved_off");
            if (!told) msg += " " + str(panel, "planes_later");
            set_status(msg, false);
            write_controls(panel);
        }
    }

    /** [STYLES 2026-11-10] A kind's save: the grid's answer, then the plugin told (planes, ships) or this viewer's birds
     *  changed now; a failure stays on the status line and in an alert. */
    void save_kind_coro(LLHandle<LLPanel> handle, LLUUID region, Kind k, std::string style, U64 serial)
    {
        LLCore::HttpStatus status;
        const LLSD reply = post_and_wait(kind_body(region, k, style), true, status);
        const bool ok = status && reply.isMap() && reply["success"].asBoolean();
        LLPanel* panel = handle.get();
        if (!ok)
        {
            LLNotificationsUtil::add("GenericAlertOK", LLSD().with("MESSAGE",
                std::string(k == KIND_PLANES ? "Could not save the kind of planes: " : k == KIND_SHIPS ? "Could not save the kind of ships: "
                            : "Could not save the kind of birds: ") + why_failed(status, reply)));
        }
        if (sState.mSerial != serial) return;
        sState.mBusy = false;
        if (!ok)
        {
            if (panel)
            {
                set_status(str(panel, "kind_save_failed") + " " + why_failed(status, reply), true);
                write_controls(panel);   // the drop-down goes back to what the grid has
            }
            return;
        }
        sState.mReply = reply;
        sState.mHave = true;
        const bool here = WolfGrid::isOnWolfTerritoriesConfirmed() && gAgent.getRegion() && gAgent.getRegion()->getRegionID() == region;
        bool told = false;
        std::string msg;
        if (k == KIND_PLANES)
        {
            // wolf-air 1.9.0 "/74 traffic style": it reads the record at once (else at its next 30-minute read)
            if (here && !(rlv_handler_t::isEnabled() && !RlvActions::canSendChannel(AIR_CHANNEL)))
            {
                send_chat_from_viewer("traffic style", CHAT_TYPE_NORMAL, AIR_CHANNEL);
                told = true;
            }
            if (panel) msg = with_kind(str(panel, "kind_saved_planes"), kind_words(panel, k, kind_resolved(k))) + (told ? "" : " " + str(panel, "planes_later"));
        }
        else if (k == KIND_SHIPS)
        {
            // wolf-naval 1.14.0 takes boats.style from the same "get" the boats nudge asks for
            if (here && !(rlv_handler_t::isEnabled() && !RlvActions::canSendChannel(SHIPS_CHANNEL)))
            {
                send_chat_from_viewer(SHIPS_NUDGE, CHAT_TYPE_NORMAL, SHIPS_CHANNEL);
                told = true;
            }
            if (panel) msg = with_kind(str(panel, "kind_saved_ships"), kind_words(panel, k, kind_resolved(k))) + (told ? "" : " " + str(panel, "ships_later"));
        }
        else
        {
            // no plugin: the viewers draw the birds from weather.php within the minute; this one now
            if (WolfRegionWeather::instanceExists()) WolfRegionWeather::instance().setBirdsZoneLocal(region, kind_resolved(k));
            if (panel) msg = with_kind(str(panel, "kind_saved_birds"), kind_words(panel, k, kind_resolved(k)));
        }
        if (panel)
        {
            set_status(msg, false);
            write_controls(panel);
        }
    }

    void on_kind_commit(LLHandle<LLPanel> handle, Kind k)
    {
        LLPanel* panel = handle.get();
        if (!panel) return;
        LLComboBox* combo = panel->findChild<LLComboBox>(kind_combo(k));
        if (!combo) return;
        if (sState.mBusy || !sState.mHave || !sState.mReply["mayChange"].asBoolean() || !have_kind(k))
        {
            write_controls(panel);
            return;
        }
        const std::string style = combo->getValue().asString();
        if (style.empty() || style == kind_value(k)) return;   // nothing changed
        const LLUUID region = sState.mRegion;
        const U64 serial = sState.mSerial;
        sState.mBusy = true;
        set_status(str(panel, "planes_saving"), false);
        write_controls(panel);
        LLCoros::instance().launch("WolfPassingPlanesKind", [handle, region, k, style, serial]() { save_kind_coro(handle, region, k, style, serial); });
    }

    void on_commit(LLHandle<LLPanel> handle, What what)
    {
        LLPanel* panel = handle.get();
        if (!panel) return;
        LLCheckBoxCtrl* check = panel->findChild<LLCheckBoxCtrl>(
            what == WHAT_BIRDS ? "wolf_birds_check" : what == WHAT_SOUNDS ? "wolf_sounds_check"
            : what == WHAT_SHIPS ? "wolf_ships_check" : what == WHAT_SEALIFE ? "wolf_sealife_check"
            : what == WHAT_UNDERSEA ? "wolf_undersea_check" : "wolf_planes_check");
        if (!check) return;
        if (sState.mBusy || !sState.mHave || !sState.mReply["mayChange"].asBoolean() || (what == WHAT_BIRDS && !have_birds())
            || (what == WHAT_SOUNDS && !have_sounds()) || (what == WHAT_SHIPS && !have_ships())
            || (what == WHAT_SEALIFE && !have_sealife()) || (what == WHAT_UNDERSEA && !have_undersea()))
        {
            write_controls(panel);
            return;
        }
        const bool on = check->get();
        const LLUUID region = sState.mRegion;
        const U64 serial = sState.mSerial;
        sState.mBusy = true;
        set_status(str(panel, "planes_saving"), false);
        write_controls(panel);
        LLCoros::instance().launch("WolfPassingPlanesSave", [handle, region, on, what, serial]() { save_coro(handle, region, on, what, serial); });
    }
}

void WolfPassingPlanes::postBuild(LLPanel* panel)
{
    LLHandle<LLPanel> handle = panel->getHandle();
    if (LLCheckBoxCtrl* check = panel->findChild<LLCheckBoxCtrl>("wolf_planes_check"))
    {
        check->setCommitCallback([handle](LLUICtrl*, const LLSD&) { on_commit(handle, WHAT_PLANES); });
    }
    if (LLCheckBoxCtrl* ships = panel->findChild<LLCheckBoxCtrl>("wolf_ships_check"))   // [SHIP OPT-IN 2026-11-03/]
    {
        ships->setCommitCallback([handle](LLUICtrl*, const LLSD&) { on_commit(handle, WHAT_SHIPS); });
    }
    if (LLCheckBoxCtrl* birds = panel->findChild<LLCheckBoxCtrl>("wolf_birds_check"))   // [BIRDS 2026-11-02/]
    {
        birds->setCommitCallback([handle](LLUICtrl*, const LLSD&) { on_commit(handle, WHAT_BIRDS); });
    }
    if (LLCheckBoxCtrl* sounds = panel->findChild<LLCheckBoxCtrl>("wolf_sounds_check"))   // [NATURE SOUNDS 2026-11-03/]
    {
        sounds->setCommitCallback([handle](LLUICtrl*, const LLSD&) { on_commit(handle, WHAT_SOUNDS); });
    }
    if (LLCheckBoxCtrl* sealife = panel->findChild<LLCheckBoxCtrl>("wolf_sealife_check"))   // [SEA LIFE 2026-11-03/]
    {
        sealife->setCommitCallback([handle](LLUICtrl*, const LLSD&) { on_commit(handle, WHAT_SEALIFE); });
    }
    if (LLCheckBoxCtrl* undersea = panel->findChild<LLCheckBoxCtrl>("wolf_undersea_check"))   // [UNDERSEA 2026-11-09/]
    {
        undersea->setCommitCallback([handle](LLUICtrl*, const LLSD&) { on_commit(handle, WHAT_UNDERSEA); });
    }
    for (Kind k : { KIND_PLANES, KIND_SHIPS, KIND_BIRDS })   // [STYLES 2026-11-10/]
    {
        if (LLComboBox* combo = panel->findChild<LLComboBox>(kind_combo(k)))
        {
            combo->setCommitCallback([handle, k](LLUICtrl*, const LLSD&) { on_kind_commit(handle, k); });
        }
    }
}

void WolfPassingPlanes::sync(LLPanel* panel, LLViewerRegion* region, bool reread)
{
    if (!panel) return;
    // Wolf Territories' own: gone (not greyed) elsewhere, as the Region tab's Adult Content link (processRegionInfo).
    const bool wolf = WolfGrid::isOnWolfTerritories() && region;
    for (const char* name : { "wolf_planes_check", "wolf_planes_state", "wolf_planes_status",
                              "wolf_ships_check", "wolf_ships_state",   // [SHIP OPT-IN 2026-11-03/]
                              "wolf_birds_check", "wolf_birds_state",
                              "wolf_sounds_check", "wolf_sounds_state",     // [NATURE SOUNDS 2026-11-03/]
                              "wolf_sealife_check", "wolf_sealife_state",   // [SEA LIFE 2026-11-03/]
                              "wolf_undersea_check", "wolf_undersea_state",   // [UNDERSEA 2026-11-09/]
                              "wolf_planes_kind_label", "wolf_planes_kind", "wolf_ships_kind_label", "wolf_ships_kind",   // [STYLES 2026-11-10/]
                              "wolf_birds_kind_label", "wolf_birds_kind", "wolf_birds_kind_state" })
    {
        if (LLView* v = panel->findChildView(name)) v->setVisible(wolf);
    }
    if (!wolf)
    {
        layout(panel);   // [TICKET 529 2026-11-05/] the block went: the panel back to the view's size, the window to its own
        return;
    }
    const LLUUID id = region->getRegionID();
    const F64 now = LLFrameTimer::getTotalSeconds();
    if (id != sState.mRegion)
    {
        sState = State();
        sState.mRegion = id;
        sState.mSerial = (U64)LLFrameTimer::getTotalTime();   // differs from every earlier region's
    }
    const bool first = !sState.mHave && sState.mFailedAt == 0.0 && sState.mFetchedAt < 0.0;
    const bool again = reread && now - sState.mFetchedAt >= REREAD_DEBOUNCE_SEC;
    const bool retry = !sState.mHave && sState.mFailedAt > 0.0 && now - sState.mFailedAt >= RETRY_SEC;
    if (!sState.mBusy && (first || again || retry))
    {
        sState.mBusy = true;
        sState.mFetchedAt = now;
        if (!sState.mHave) set_status(str(panel, "planes_reading"), false, true);
        LLHandle<LLPanel> handle = panel->getHandle();
        const U64 serial = sState.mSerial;
        LLCoros::instance().launch("WolfPassingPlanesRead", [handle, id, serial]() { fetch_coro(handle, id, serial); });
    }
    write_controls(panel);
}
