/**
 * @file wolffloateradventurestudioedit.cpp
 * @brief WolfViewer: the full Adventure Studio editors (P3). See wolffloateradventurestudio.h.
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

/*
 * [WOLF ADVENTURE P3 2026-10-16] STUDIO_AND_CONSOLE_V1.md §2.2-2.9 and §6: Outline, Cast, Places (with the
 * in-world tool WolfToolAdventurePlace), Story (scene and beat editors, the trigger list in plain
 * sentences), Rules (preset cards and the override editor), Lore (story and source cards, encounters,
 * rewards, handouts, learning, quizzes) and Test Play (with the debug overlay).
 *
 * HOW IT EDITS. Every editor writes the open draft (mDraftJson) through editDraft, which the P2 code saves
 * 2 s after the last change (STUDIO §1 "Saving is automatic") with the 409 Reload / Keep mine handling.
 * The brain is the only validator (§1): the drawer lists its `problems`, a click on one opens the page
 * and focuses the field at its JSON pointer, and a field with an error gets the error mark (a red rim
 * and the words beside it, §6 3.3.1).
 *
 * THE FORMS come from the pack's own schema (app_settings/wolfadventure/adventure_pack_v1.schema.json,
 * copied from region-terrain-work/wolf-story/spec/), so every field the format has is editable with the
 * type, limits and choices the format gives it: text with its length counter, numbers in their range,
 * yes/no switches, choices, lists with Add / Move up / Move down / Duplicate / Delete (the single-pointer
 * alternative to dragging, §6 2.5.7), references as pickers of what the pack already has, and the
 * condition builder and effect list of §2.8. The rules editor is the same form over the preset with the
 * pack's overrides merged; every change is written back as an RFC 7396 merge patch (RULES_PRESETS_V1.md §3).
 *
 * KNOWN P3 LIMITS (declared, not hidden):
 *  - Changing the preset asks first and lists the stats and checks the pack uses that the new preset lacks; it does
 *    not show the whole difference of the merged rules. rules.presetVersion is pinned by the brain on publish.
 *  - Keep mine (a 409 conflict) saves over the other window's draft without first showing that draft's problems.
 *  - test_start's regionName / here and the test_state action are P3 additions to BRAIN_API_V1.md §5.5 (the lead
 *    records them in the spec).
 *  - Text fields edit at the end only (type, Backspace, paste): there is no caret movement or selection in P3.
 */

#include "llviewerprecompiledheaders.h"

#include "wolffloateradventurestudio.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <fstream>
#include <sstream>
#include <utility>

#include <boost/json.hpp>

#include "llagent.h"
#include "llbase64.h"
#include "llclipboard.h"
#include "lldir.h"
#include "llfilepicker.h"
#include "llfontgl.h"
#include "llinventory.h"
#include "lllocalcliprect.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llsdjson.h"
#include "lltexturectrl.h"
#include "lltooldraganddrop.h"
#include "lltoolmgr.h"
#include "lltoolpie.h"
#include "llurldispatcher.h"
#include "llviewermenufile.h"
#include "llviewerregion.h"
#include "llviewertexturelist.h"
#include "llviewerwindow.h"
#include "llweb.h"
#include "llworldmap.h"
#include "wolfadventure.h"
#include "wolfgame.h"
#include "wolfgamehud.h"
#include "wolftooladventureplace.h"
#include "wolfadventurestudioutil.h"

using namespace WolfGameDraw;

namespace
{
    F64 now() { return LLFrameTimer::getTotalSeconds(); }
    LLColor4 fade(const LLColor4& c, F32 a) { return LLColor4(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], c.mV[VALPHA] * a); }
    const LLColor4 DANGER_INK(0.988f, 0.647f, 0.647f, 1.f);   // #fca5a5, as wolffloateradventurestudio.cpp
    const LLColor4 OK_INK(0.290f, 0.871f, 0.502f, 1.f);       // #4ade80
    const LLColor4 WARN_INK(0.984f, 0.749f, 0.141f, 1.f);     // #fbbf24
    const F32 LABEL_W = 150.f;                                 // the label column of a form row
    const F32 ROW_H = 26.f;                                    // a one-line control (≥ 24, HUD §6 2.5.8)
    const F64 EDIT_IDLE_SEC = 0.8;                             // a field is written 0.8 s after the last key
    std::string cap(const std::string& s, size_t n) { return WolfGame::cleanText(s, n); }

    // ── JSON pointers (RFC 6901) ─────────────────────────────────────────────────────────────────────────────
    std::vector<std::string> ptrTokens(const std::string& ptr)
    {
        std::vector<std::string> out;
        if (ptr.empty()) return out;
        std::string tok;
        for (size_t i = 1; i <= ptr.size(); ++i)
        {
            if (i == ptr.size() || ptr[i] == '/')
            {
                std::string t;
                for (size_t k = 0; k < tok.size(); ++k)
                {
                    if (tok[k] == '~' && k + 1 < tok.size()) { t += tok[k + 1] == '1' ? '/' : '~'; ++k; }
                    else t += tok[k];
                }
                out.push_back(t);
                tok.clear();
            }
            else tok += ptr[i];
        }
        return out;
    }
    std::string ptrEscape(const std::string& k)
    {
        std::string o;
        for (char c : k) { if (c == '~') o += "~0"; else if (c == '/') o += "~1"; else o += c; }
        return o;
    }
    std::string ptrJoin(const std::string& base, const std::string& key) { return base + "/" + ptrEscape(key); }
    std::string ptrIndex(const std::string& base, S32 i) { return base + llformat("/%d", i); }
    std::string ptrParent(const std::string& ptr)
    {
        const size_t s = ptr.rfind('/');
        return s == std::string::npos || s == 0 ? std::string() : ptr.substr(0, s);
    }
    std::string ptrLast(const std::string& ptr)
    {
        const std::vector<std::string> t = ptrTokens(ptr);
        return t.empty() ? std::string() : t.back();
    }
    bool isIndex(const std::string& t) { return !t.empty() && std::all_of(t.begin(), t.end(), [](char c) { return c >= '0' && c <= '9'; }); }
    /** A pointer segment as an array index, or -1 (not digits, or too long to be one: no list holds 1,000,000 entries). */
    S32 tokIndex(const std::string& t) { return isIndex(t) && t.size() <= 6 ? (S32)strtol(t.c_str(), nullptr, 10) : -1; }

    const LLSD& sdAt(const LLSD& root, const std::string& ptr)
    {
        static const LLSD undef;
        const LLSD* v = &root;
        for (const std::string& t : ptrTokens(ptr))
        {
            if (v->isArray()) { const S32 i = tokIndex(t); if (i < 0 || i >= (S32)v->size()) return undef; v = &(*v)[(LLSD::Integer)i]; }
            else if (v->isMap()) { if (!v->has(t)) return undef; v = &(*v)[t]; }
            else return undef;
        }
        return *v;
    }

    /** The value at `ptr`; with `create`, missing members along the way are made as objects (or an array before an
     *  index) - never array items, and never over a value that is there but is not an object (that is a stale pointer). */
    boost::json::value* jAt(boost::json::value& root, const std::string& ptr, bool create)
    {
        boost::json::value* v = &root;
        const std::vector<std::string> t = ptrTokens(ptr);
        for (size_t i = 0; i < t.size(); ++i)
        {
            if (v->is_array())
            {
                const S32 k = tokIndex(t[i]);
                if (k < 0 || k >= (S32)v->as_array().size()) return nullptr;
                v = &v->as_array()[(size_t)k];
                continue;
            }
            if (!v->is_object()) return nullptr;
            boost::json::object& o = v->as_object();
            if (!o.contains(t[i]))
            {
                if (!create) return nullptr;
                o[t[i]] = (i + 1 < t.size() && isIndex(t[i + 1])) ? boost::json::value(boost::json::array()) : boost::json::value(boost::json::object());
            }
            v = &o[t[i]];
        }
        return v;
    }
    bool jErase(boost::json::value& root, const std::string& ptr)
    {
        boost::json::value* parent = jAt(root, ptrParent(ptr), false);
        if (!parent) return false;
        const std::string last = ptrLast(ptr);
        if (parent->is_array())
        {
            boost::json::array& a = parent->as_array();
            const S32 k = tokIndex(last);
            if (k < 0 || k >= (S32)a.size()) return false;
            a.erase(a.begin() + k);
            return true;
        }
        if (parent->is_object()) return parent->as_object().erase(last) > 0;
        return false;
    }

    // ── the schemas ─────────────────────────────────────────────────────────────────────────────────────────
    boost::json::value loadJson(const std::string& path)
    {
        std::ifstream in(path.c_str(), std::ios::binary);
        if (!in) return boost::json::value();
        std::stringstream ss;
        ss << in.rdbuf();
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(ss.str(), ec);
        return ec ? boost::json::value() : v;
    }
    const boost::json::value& packSchema()
    {
        static const boost::json::value s = loadJson(gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "wolfadventure", "adventure_pack_v1.schema.json"));
        return s;
    }
    const boost::json::value& rulesSchema()
    {
        static const boost::json::value s = loadJson(gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "wolfadventure", "rules", "rules_preset_v1.schema.json"));
        return s;
    }
    const boost::json::value& nullV() { static const boost::json::value n; return n; }
    /** A $defs entry of a schema root, or null. */
    const boost::json::value& def(const boost::json::value& root, const std::string& name)
    {
        if (!root.is_object()) return nullV();
        const boost::json::value* d = root.as_object().if_contains("$defs");
        if (!d || !d->is_object()) return nullV();
        const boost::json::value* x = d->as_object().if_contains(name);
        return x ? *x : nullV();
    }
    /** Follows $ref (only "#/$defs/..." is used by our schemas); `name` gets the def's name. */
    const boost::json::value& resolve(const boost::json::value& root, const boost::json::value& s, std::string* name = nullptr)
    {
        const boost::json::value* cur = &s;
        for (S32 guard = 0; guard < 8 && cur->is_object(); ++guard)
        {
            const boost::json::value* r = cur->as_object().if_contains("$ref");
            if (!r || !r->is_string()) break;
            const std::string ref(r->as_string().c_str());
            const std::string pre = "#/$defs/";
            if (ref.rfind(pre, 0) != 0) break;
            if (name) *name = ref.substr(pre.size());
            cur = &def(root, ref.substr(pre.size()));
        }
        return *cur;
    }
    const boost::json::value& prop(const boost::json::value& s, const std::string& key)
    {
        if (!s.is_object()) return nullV();
        const boost::json::value* p = s.as_object().if_contains("properties");
        if (!p || !p->is_object()) return nullV();
        const boost::json::value* x = p->as_object().if_contains(key);
        return x ? *x : nullV();
    }
    std::vector<std::string> propKeys(const boost::json::value& s)
    {
        std::vector<std::string> out;
        if (!s.is_object()) return out;
        const boost::json::value* p = s.as_object().if_contains("properties");
        if (!p || !p->is_object()) return out;
        for (const auto& kv : p->as_object()) out.push_back(std::string(kv.key()));
        return out;
    }
    bool isRequired(const boost::json::value& s, const std::string& key)
    {
        if (!s.is_object()) return false;
        const boost::json::value* r = s.as_object().if_contains("required");
        if (!r || !r->is_array()) return false;
        for (const auto& x : r->as_array()) if (x.is_string() && std::string(x.as_string().c_str()) == key) return true;
        return false;
    }
    std::string sStr(const boost::json::value& s, const char* key)
    {
        if (!s.is_object()) return std::string();
        const boost::json::value* x = s.as_object().if_contains(key);
        return x && x->is_string() ? std::string(x->as_string().c_str()) : std::string();
    }
    bool sNum(const boost::json::value& s, const char* key, F64& out)
    {
        if (!s.is_object()) return false;
        const boost::json::value* x = s.as_object().if_contains(key);
        if (!x) return false;
        if (x->is_int64()) { out = (F64)x->as_int64(); return true; }
        if (x->is_uint64()) { out = (F64)x->as_uint64(); return true; }
        if (x->is_double()) { out = x->as_double(); return true; }
        return false;
    }
    /** The schema's "type" (the first, when it is a list), "" when none. */
    std::string sType(const boost::json::value& s)
    {
        if (!s.is_object()) return std::string();
        const boost::json::value* t = s.as_object().if_contains("type");
        if (!t) return std::string();
        if (t->is_string()) return std::string(t->as_string().c_str());
        if (t->is_array() && !t->as_array().empty() && t->as_array()[0].is_string()) return "mixed";
        return std::string();
    }
    std::vector<std::string> sEnum(const boost::json::value& s)
    {
        std::vector<std::string> out;
        if (!s.is_object()) return out;
        const boost::json::value* e = s.as_object().if_contains("enum");
        if (!e || !e->is_array()) return out;
        for (const auto& x : e->as_array()) if (x.is_string()) out.push_back(std::string(x.as_string().c_str()));
        return out;
    }
    /** [WOLF ADVENTURE AUDIT 2026-10-17] The schema's enum when its members are numbers (rules_preset_v1.schema.json
     *  dice.sides items: {"enum": [2, 3, 4, 6, 8, 10, 12, 20, 100]}); sEnum above keeps only the text members. */
    std::vector<S64> sEnumNums(const boost::json::value& s)
    {
        std::vector<S64> out;
        if (!s.is_object()) return out;
        const boost::json::value* e = s.as_object().if_contains("enum");
        if (!e || !e->is_array()) return out;
        for (const auto& x : e->as_array())
        {
            if (x.is_int64()) out.push_back(x.as_int64());
            else if (x.is_uint64()) out.push_back((S64)x.as_uint64());
        }
        return out;
    }
    /** [WOLF ADVENTURE AUDIT 2026-10-17] True when a schema describes an object to draw as a group of fields: its type
     *  is "object", or it has no type but has members (properties / additionalProperties). Every schema node is a JSON
     *  object, so `s.is_object()` alone is true for a text field too - that test drew each text field as a stats list
     *  ("Add a stat") and made new items' text as {} (Paul's 2026-10-17 screenshot: "must be text /acts/1/title"). */
    bool isObjectSchema(const boost::json::value& s, const std::string& type)
    {
        if (type == "object") return true;
        if (!type.empty() || !s.is_object()) return false;
        const boost::json::object& o = s.as_object();
        return o.contains("properties") || o.contains("additionalProperties") || o.contains("oneOf");
    }
    /** The schema's default, as LLSD (undefined when none). */
    LLSD sDefault(const boost::json::value& s)
    {
        if (!s.is_object()) return LLSD();
        const boost::json::value* d = s.as_object().if_contains("default");
        return d ? LlsdFromJson(*d) : LLSD();
    }
    std::string sDescription(const boost::json::value& s) { return sStr(s, "description"); }
    boost::json::value toJson(const LLSD& v) { return LlsdToJson(v); }

    // ── words ───────────────────────────────────────────────────────────────────────────────────────────────
    /** A field's label in words (STUDIO §2.3 / §2.5 names where the spec gives them; otherwise the key in words). */
    std::string labelFor(const std::string& key)
    {
        static const std::map<std::string, std::string> L = {
            { "id", "Id" }, { "title", "Title" }, { "summary", "Summary" }, { "purpose", "Purpose" }, { "rating", "Rating" },
            { "language", "Language" }, { "tags", "Tags" }, { "regions", "Regions" }, { "region", "Region" }, { "npcBudget", "Characters at once (budget)" },
            { "party", "Party" }, { "mode", "Mode" }, { "minSize", "Fewest players" }, { "maxSize", "Most players" }, { "join", "Who may join" },
            { "repeat", "May play again" }, { "run", "Running" }, { "auto", "Runs by itself" }, { "liveGm", "A host may take over" },
            { "callGm", "Players may call a host" }, { "schedule", "Schedule" }, { "start", "How it starts" }, { "beat", "Beat" },
            { "trigger", "Started by" }, { "cast", "Character" }, { "area", "Area" }, { "intro", "Intro" }, { "experiences", "Wolf Experiences listing" },
            { "list", "List it" }, { "category", "Category" }, { "nowLine", "Show what is happening now" }, { "credits", "Credits" },
            { "role", "Role" }, { "name", "Name" }, { "attribution", "Attribution" }, { "work", "Work" }, { "text", "Text" },
            { "licence", "Licence" }, { "url", "Web address" }, { "author", "Author" }, { "learning", "Learning" }, { "goals", "Goals" },
            { "keyFacts", "Key facts (source cards)" }, { "quiz", "Quiz" }, { "xapiActivity", "xAPI activity" }, { "first", "First name" },
            { "last", "Last name" }, { "portrait", "Portrait (texture)" }, { "look", "Look" }, { "appearance", "Appearance notecard" },
            { "home", "Home spot" }, { "persona", "Persona" }, { "manner", "Manner" }, { "knows", "What they know" },
            { "neverReveal", "Secrets - never revealed" }, { "neverSay", "Never says" }, { "cards", "Story cards they may know" },
            { "greeting", "Greeting" }, { "idle", "Idle lines" }, { "barks", "Barks" }, { "voice", "Voice" }, { "rate", "Speed" },
            { "actions", "May do" }, { "triggerIds", "Story moments they may make happen" }, { "actor", "Fights" }, { "health", "Health" },
            { "level", "Level" }, { "hostility", "Hostility" }, { "sub", "Subtitle" }, { "icon", "Icon" }, { "loot", "Loot (reward)" },
            { "statBlock", "5E stat block" }, { "source", "Source" }, { "spots", "Spots" }, { "areas", "Areas" }, { "paths", "Paths" },
            { "chests", "Chests" }, { "label", "Label" }, { "pos", "Position (x, y, z)" }, { "facing", "Facing (degrees)" }, { "shape", "Shape" },
            { "zMin", "Lowest z" }, { "zMax", "Highest z" }, { "points", "Points" }, { "loop", "Loops" }, { "gait", "Gait" },
            { "pause", "Pause at each point (s)" }, { "objective", "Objective" }, { "marker", "Map marker" }, { "onDuty", "On duty" },
            { "goal", "Goal" }, { "lines", "Written lines" }, { "replies", "Quick replies" }, { "say", "What the player says" },
            { "onEnter", "When this starts" }, { "decisions", "Decisions" }, { "triggers", "Triggers" }, { "hints", "Hints" },
            { "after", "After (seconds)" }, { "by", "Said by" }, { "recover", "If they wander off" }, { "do", "Do" },
            { "aiNotes", "Notes for the AI Director" }, { "shared", "Shared world" }, { "stage", "Stage" }, { "dismiss", "Dismiss" },
            { "encounters", "Encounters" }, { "ambience", "Ambience" }, { "environment", "Environment (Wolf Environment patch, JSON)" },
            { "scope", "Scope" }, { "music", "Music" }, { "volume", "Volume" }, { "sounds", "Sounds" }, { "every", "Repeat every (s)" },
            { "sound", "Sound" }, { "boxed", "Boxed text (read aloud)" }, { "gmNotes", "Host notes" }, { "handouts", "Handouts" },
            { "entry", "Entry rule" }, { "exits", "Exits" }, { "to", "To" }, { "if", "Only if" }, { "prompt", "Prompt" }, { "image", "Image (texture)" },
            { "who", "Who" }, { "sec", "Seconds" }, { "options", "Options" }, { "detail", "Detail" }, { "once", "Once" }, { "next", "Then go to" },
            { "intent", "Words that count" }, { "object", "Object name" }, { "decision", "Decision" }, { "option", "Option" },
            { "reply", "Quick reply" }, { "item", "Item" }, { "qty", "How many" }, { "hours", "Hours (from, to)" }, { "check", "Check" },
            { "dc", "Difficulty (DC)" }, { "result", "Outcome" }, { "passed", "Passed" }, { "scene", "Scene" }, { "titleCard", "Chapter card" },
            { "scenes", "Scenes" }, { "beats", "Beats" }, { "outcome", "Outcome" }, { "reward", "Reward" }, { "keys", "Keys" },
            { "always", "Always included" }, { "priority", "Priority" }, { "gmOnly", "Hosts only" }, { "citation", "Citation" }, { "date", "Date" },
            { "where", "Where (page, chapter)" }, { "era", "Latest year it speaks about" }, { "members", "Members" }, { "count", "Copies" },
            { "offset", "Offset (x, y, z)" }, { "props", "Props" }, { "chest", "Chest" }, { "rot", "Turn (degrees)" }, { "despawnAfter", "Remove after (s)" },
            { "combat", "Turn-based combat" }, { "items", "Items" }, { "money", "Money" }, { "stats", "Stats" }, { "inventory", "Inventory items" },
            { "kind", "Kind" }, { "texture", "Picture (texture)" }, { "passMark", "Pass mark (0-1)" }, { "type", "Type" }, { "choices", "Choices" },
            { "maxChoices", "Most choices" }, { "correct", "Correct choices" }, { "accept", "Accepted answers" }, { "points", "Points" },
            { "dailyCapCredits", "Daily cap (credits)" }, { "flags", "Flags" }, { "factions", "Factions" }, { "honour", "Honour" },
            { "npcReputation", "Characters' opinions of players" }, { "bands", "Bands" }, { "from", "From" }, { "colour", "Colour" },
            { "scope", "Scope" }, { "default", "Starts as" }, { "min", "Lowest" }, { "max", "Highest" }, { "maxLength", "Longest text" },
            { "scriptRead", "Scripts may read it" }, { "scriptWrite", "Scripts may write it" }, { "journal", "Journal label" },
            { "enabled", "On" }, { "endsOn", "Ends on" }, { "followSchedule", "Follows their schedule" }, { "anim", "Animation" }, { "cue", "Cue" },
            { "on", "On" }, { "spot", "Spot" }, { "path", "Path" }, { "at", "At spot" }, { "absent", "Away" }, { "when", "When" }, { "line", "Line" },
            { "audio", "Pre-recorded sound" }, { "emote", "Emote" }, { "public", "Public" }, { "statBlock", "5E stat block" },
            // [WOLF ADVENTURE COMBAT 2026-10-17] COMBAT_V1.md §1
            { "enemies", "Enemies" }, { "battles", "Battles" }, { "enemy", "Enemy" }, { "battle", "Battle" }, { "description", "Description" },
            { "behaviour", "How it fights" }, { "fleeAt", "Runs away at (% health left)" }, { "hp", "Health" }, { "defence", "Defence" },
            { "attack", "Attack bonus" }, { "damage", "Damage (dice, e.g. 1d6+1)" }, { "initiative", "Initiative bonus" }, { "spread", "Spread (metres)" },
            { "mayFlee", "Players may flee" }, { "onLoss", "If the party loses" }, { "share", "Who gets each enemy's loot" },
            { "aggressive", "Attacks every turn" }, { "defensive", "Defends until hurt" }, { "flees", "Runs away when hurt" },
            { "retry", "Try again (full health)" }, { "each", "Every player" }, { "killer", "The one who beat it" } };
        auto it = L.find(key);
        if (it != L.end()) return it->second;
        // camelCase in words
        std::string out;
        for (size_t i = 0; i < key.size(); ++i)
        {
            const char c = key[i];
            if (i == 0) out += (char)toupper(c);
            else if (isupper((unsigned char)c)) { out += ' '; out += (char)tolower(c); }
            else out += c;
        }
        return out;
    }

    /** The kind of thing an id names, from its key (the pack's cross-reference rules, ADVENTURE_PACK_V1.md §15). */
    std::string refKind(const std::string& key, const std::string& parentKey)
    {
        if (key == "cast" || key == "by" || key == "castRep") return "cast";
        if (key == "spot" || key == "home" || key == "marker" || key == "at") return "spot";
        if (key == "area") return "area";
        if (key == "path") return "path";
        if (key == "chest") return "chest";
        if (key == "beat" || key == "next") return "beat";
        if (key == "scene" || (key == "to" && parentKey == "exits")) return "scene";
        if (key == "decision") return "decision";
        if (key == "option") return "option";
        if (key == "reply") return "reply";
        if (key == "reward" || key == "loot") return "parcel";
        if (key == "end") return "ending";
        if (key == "handout") return "handout";
        if (key == "quiz") return "quiz";
        if (key == "encounter" || key == "spawn") return "encounter";
        if (key == "flag") return "flag";
        if (key == "faction" || key == "rep") return "faction";
        if (key == "band") return "band";
        if (key == "visited") return "visited";
        if (key == "check") return "check";
        if (key == "source") return "sourceCard";
        if (key == "statBlock") return "statBlock";
        if (key == "enemy") return "enemy";     // [WOLF ADVENTURE COMBAT 2026-10-17] COMBAT_V1.md §1.2 battles[].enemies[].enemy
        if (key == "battle") return "battle";   // §1.3 effect battle, triggers won / lost
        return std::string();
    }
    /** The kind an array of ids holds, from its key. */
    std::string listKind(const std::string& key)
    {
        if (key == "dismiss") return "cast";
        if (key == "encounters") return "encounter";
        if (key == "handouts") return "handout";
        if (key == "cards") return "storyCard";
        if (key == "keyFacts") return "sourceCard";
        if (key == "triggerIds") return "aiTrigger";
        if (key == "correct") return "choice";
        return std::string();
    }

    // Source: ADVENTURE_PACK_V1.md §11.2 (the ops) and STUDIO_AND_CONSOLE_V1.md §2.8 ("Add step... offers every
    // op of §11.2 as a plain sentence").
    struct Op { const char* mOp; const char* mSentence; };
    const Op OPS[] = {
        { "set", "Set a flag" }, { "add", "Add to a counter" }, { "rep", "Change reputation" }, { "honour", "Change honour by" },
        { "goto", "Go to a beat or scene" }, { "end", "End the story with an ending" }, { "reward", "Give a reward" },
        { "take", "Take items from the player" }, { "journal", "Add a journal entry" }, { "vignette", "Show a narration card" },
        { "say", "A character says a line" }, { "spawn", "Spawn an encounter" }, { "despawn", "Remove an encounter or a character" },
        { "move", "Move a character" }, { "anim", "Play an animation" }, { "stage", "A stage cue (fade, title, camera)" },
        { "ambience", "Change the ambience" }, { "sound", "Play a sound at a spot" }, { "marker", "Set or clear the map marker" },
        { "notify", "Show a notice" }, { "handout", "Give a handout" }, { "roll", "Ask for a roll" }, { "combat", "Start or stop turn-based combat" },
        { "hint", "Give a hint" }, { "callGm", "Call a host" }, { "quiz", "Show a quiz" }, { "wait", "Wait some seconds" },
        { "battle", "Start a battle" } };   // [WOLF ADVENTURE COMBAT 2026-10-17] COMBAT_V1.md §1.3
    const char* opSentence(const std::string& op)
    {
        for (const Op& o : OPS) if (op == o.mOp) return o.mSentence;
        return "A step";
    }

    // Source: STUDIO_AND_CONSOLE_V1.md §2.5 "Add trigger... opens a list of plain sentences, one per type
    // (ADVENTURE_PACK_V1.md §11.1)", and the §11.1 table's "Needs" column for the fields of each type.
    struct TriggerType { const char* mOn; const char* mSentence; const char* mFields; };
    const TriggerType TRIGGERS[] = {
        { "arrive", "When a player arrives at... (an area)", "area" },
        { "leave", "When a player leaves... (an area)", "area" },
        { "talk", "When a player talks to... (a character) [and picks the reply...]", "cast,reply" },
        { "say", "When a player says something meaning... (words)", "cast,intent" },
        { "touch", "When a player touches... (an object name)", "object" },
        { "choose", "When the party chooses... (a decision) [option...]", "decision,option" },
        { "give", "When a player gives... (an item) to...", "cast,item,qty" },
        { "defeat", "When... is defeated", "cast" },
        { "timer", "After... seconds", "sec" },
        { "flag", "When... becomes true", "" },
        { "gm", "When the host presses...", "label" },
        { "script", "When a script fires...", "" },
        { "roll", "When a roll for... succeeds or fails", "check,dc,result" },
        { "item", "When a player holds... of...", "item,qty" },
        { "sit", "When a player sits on...", "object" },
        { "time", "Between... and... o'clock", "hours" },
        { "quiz", "When a player finishes the quiz...", "quiz,passed" },
        { "ready", "When everyone is ready", "" },
        { "ai", "When the character decides (AI)...", "cast" },
        { "scene", "When the party enters the scene...", "scene" },
        // [WOLF ADVENTURE COMBAT 2026-10-17] COMBAT_V1.md §1.3
        { "won", "When the party wins the battle...", "battle" },
        { "lost", "When the party loses the battle...", "battle" } };
    const TriggerType* triggerType(const std::string& on)
    {
        for (const TriggerType& t : TRIGGERS) if (on == t.mOn) return &t;
        return nullptr;
    }
    /** A trigger type this viewer's schema lists (adventure_pack_v1.schema.json trigger.on enum): newer ones wait for it. */
    bool triggerKnown(const std::string& on)
    {
        const boost::json::value& ts = def(packSchema(), "trigger");
        for (const std::string& e : sEnum(prop(ts, "on"))) if (e == on) return true;
        return false;
    }
    std::vector<std::string> splitComma(const std::string& s)
    {
        std::vector<std::string> out;
        std::string cur;
        for (char c : s) { if (c == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); } else cur += c; }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }
    /** [WOLF ADVENTURE AUDIT 2026-10-17] The width a drawn button needs for its label: WolfGameDraw::button draws in
     *  SansSerifSmallBold inside 10 px of padding (wolfgamehud.cpp button(): textL max width r - l - 10), so a width
     *  measured in the regular small face cut labels ("Add sce...", "Advent...", "Modera..."). */
    F32 btnW(const std::string& label, F32 min_w = 0.f)
    {
        return llmax(min_w, (F32)LLFontGL::getFontSansSerifSmallBold()->getWidthF32(label) + 24.f);
    }
    std::string trim(const std::string& s)
    {
        size_t a = 0, b = s.size();
        while (a < b && isspace((unsigned char)s[a])) ++a;
        while (b > a && isspace((unsigned char)s[b - 1])) --b;
        return s.substr(a, b - a);
    }
    /** A value as the text a field shows. */
    std::string valueText(const LLSD& v)
    {
        if (v.isUndefined()) return std::string();
        if (v.isBoolean()) return v.asBoolean() ? "true" : "false";
        if (v.isInteger()) return llformat("%d", v.asInteger());
        if (v.isReal())
        {
            std::string s = llformat("%.4f", v.asReal());
            while (!s.empty() && s.back() == '0') s.pop_back();
            if (!s.empty() && s.back() == '.') s.pop_back();
            return s;
        }
        if (v.isArray())
        {
            std::string s;
            for (size_t i = 0; i < (size_t)v.size(); ++i) s += (i ? ", " : "") + v[(LLSD::Integer)i].asString();
            return s;
        }
        if (v.isMap()) return LlsdToJson(v).is_object() ? boost::json::serialize(LlsdToJson(v)) : std::string("{}");
        return v.asString();
    }
    /** The pointer of the beat (/acts/i/scenes/j/beats/k) or scene a pointer is inside, or "". */
    std::string ptrUpTo(const std::string& ptr, const std::string& key, size_t depth)
    {
        // depth: tokens after the key's index to keep (0 = just /key/i)
        const std::vector<std::string> t = ptrTokens(ptr);
        std::string out;
        for (size_t i = 0; i < t.size(); ++i)
        {
            out += "/" + ptrEscape(t[i]);
            if (t[i] == key && i + 1 < t.size() && isIndex(t[i + 1]))
            {
                out += "/" + t[i + 1];
                (void)depth;
                return out;
            }
        }
        return std::string();
    }
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// the document: the draft (doc 0) and the rules with the overrides merged (doc 1)
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

S32 WolfAdventureStudioView::act(const std::function<void()>& fn)
{
    EditAct a;
    a.mFn = fn;
    mActs.push_back(a);
    return (S32)mActs.size() - 1;
}

S32 WolfAdventureStudioView::textAct(const std::string& ptr, S32 doc, size_t capn, char type, bool optional, F64 mn, F64 mx)
{
    EditAct a;
    a.mPtr = ptr;
    a.mDoc = doc;
    a.mCap = capn;
    a.mType = type;
    a.mOptional = optional;
    a.mMin = mn;
    a.mMax = mx;
    mActs.push_back(a);
    return (S32)mActs.size() - 1;
}

const LLSD& WolfAdventureStudioView::docRead(S32 doc) const { return doc == 1 ? mRulesMerged : doc == 2 ? mTestFlags : mDraftPack; }

namespace
{
    const char* STALE = "That item has changed or gone (another edit moved it) - nothing was written. Look again and retry.";
    /** An append target: a list, or nothing there yet (an absent member jAt just made, or null). */
    bool asList(boost::json::value* arr)
    {
        if (!arr) return false;
        if (arr->is_array()) return true;
        if (arr->is_null() || (arr->is_object() && arr->as_object().empty())) { *arr = boost::json::array(); return true; }
        return false;
    }
}

void WolfAdventureStudioView::stopPlacing()
{
    WolfToolAdventurePlace* tool = WolfToolAdventurePlace::getInstance();
    if (tool->mode() != WolfToolAdventurePlace::M_NONE) tool->cancel();
}

void WolfAdventureStudioView::docSet(S32 doc, const std::string& ptr, const boost::json::value& v)
{
    if (doc == 1) { rulesSet(ptr, &v); return; }
    if (!editDoc([&ptr, &v](boost::json::value& root)
    {
        boost::json::value* at = jAt(root, ptr, true);
        if (!at) return false;
        *at = v;
        return true;
    }) && !mDraftJson.empty()) setStatus(STALE, true);
}

void WolfAdventureStudioView::docErase(S32 doc, const std::string& ptr)
{
    if (doc == 1) { rulesSet(ptr, nullptr); return; }
    stopPlacing();
    if (!editDoc([&ptr](boost::json::value& root) { return jErase(root, ptr); }) && !mDraftJson.empty()) setStatus(STALE, true);
}

void WolfAdventureStudioView::docAppend(S32 doc, const std::string& ptr, const boost::json::value& v)
{
    if (doc == 1)
    {
        boost::json::value merged = toJson(mRulesMerged);
        boost::json::value* arr = jAt(merged, ptr, true);
        if (!asList(arr)) { setStatus(STALE, true); return; }
        arr->as_array().push_back(v);
        rulesSet(ptr, arr);
        return;
    }
    if (!editDoc([&ptr, &v](boost::json::value& root)
    {
        boost::json::value* arr = jAt(root, ptr, true);
        if (!asList(arr)) return false;
        arr->as_array().push_back(v);
        return true;
    }) && !mDraftJson.empty()) setStatus(STALE, true);
}

void WolfAdventureStudioView::docMove(S32 doc, const std::string& arrPtr, S32 index, S32 by)
{
    auto move = [arrPtr, index, by](boost::json::value& r) -> boost::json::value*
    {
        boost::json::value* arr = jAt(r, arrPtr, false);
        if (!arr || !arr->is_array()) return nullptr;
        boost::json::array& a = arr->as_array();
        const S32 to = index + by;
        if (index < 0 || to < 0 || index >= (S32)a.size() || to >= (S32)a.size()) return nullptr;
        std::swap(a[index], a[to]);
        return arr;
    };
    if (doc == 1)
    {
        boost::json::value merged = toJson(mRulesMerged);
        if (boost::json::value* arr = move(merged)) rulesSet(arrPtr, arr);
        else setStatus(STALE, true);
        return;
    }
    stopPlacing();
    if (!editDoc([&move](boost::json::value& root) { return move(root) != nullptr; }))
    {
        if (!mDraftJson.empty()) setStatus(STALE, true);
        return;
    }
    // the open card follows its row
    const std::string from = ptrIndex(arrPtr, index), to = ptrIndex(arrPtr, index + by);
    const bool a = mOpen.count(from) > 0, b = mOpen.count(to) > 0;
    mOpen.erase(from); mOpen.erase(to);
    if (a) mOpen.insert(to);
    if (b) mOpen.insert(from);
    if (mSel == from) mSel = to;
    else if (mSel == to) mSel = from;
}

namespace
{
    // Every "id" inside a copied object gets a new unique value, and the references inside the copy
    // (next, goto/beat, scene, decision) follow, so a duplicated scene still hangs together.
    // A cast sheet's voice.id names a TTS voice (BRAIN_API_V1.md §5.5 voices), not a pack item: never renamed.
    bool notPackIds(const boost::json::string_view& key) { return key == "voice"; }
    void collectIds(const boost::json::value& v, std::vector<std::string>& out)
    {
        if (v.is_object())
        {
            for (const auto& kv : v.as_object())
            {
                if (notPackIds(kv.key())) continue;
                if (kv.key() == "id" && kv.value().is_string()) out.push_back(std::string(kv.value().as_string().c_str()));
                else collectIds(kv.value(), out);
            }
        }
        else if (v.is_array()) for (const auto& x : v.as_array()) collectIds(x, out);
    }
    void renameIds(boost::json::value& v, const std::map<std::string, std::string>& m)
    {
        // the references a copy may hold to its own items (ADVENTURE_PACK_V1.md §15): trigger next, goto beat /
        // scene, a decision, its option, a talk reply
        static const std::set<std::string> REFS = { "id", "next", "beat", "scene", "decision", "option", "reply" };
        if (v.is_object())
        {
            for (auto& kv : v.as_object())
            {
                if (notPackIds(kv.key())) continue;
                const std::string k(kv.key());
                if (REFS.count(k) && kv.value().is_string())
                {
                    auto it = m.find(std::string(kv.value().as_string().c_str()));
                    if (it != m.end()) kv.value() = it->second;
                }
                else renameIds(kv.value(), m);
            }
        }
        else if (v.is_array()) for (auto& x : v.as_array()) renameIds(x, m);
    }
    std::string uniqueId(const std::string& base, const std::string& packText)
    {
        // ids are unique within their own list; a pack-wide check keeps them unique everywhere
        std::string b = base.substr(0, 40);
        if (!b.empty() && packText.find("\"id\":\"" + b + "\"") == std::string::npos) return b;   // free as it is
        for (S32 n = 2; n < 1000; ++n)
        {
            const std::string c = b + llformat("-%d", n);
            if (packText.find("\"id\":\"" + c + "\"") == std::string::npos) return c;
        }
        return b + "-x";
    }
}

void WolfAdventureStudioView::docDuplicate(S32 doc, const std::string& arrPtr, S32 index)
{
    if (doc == 1)
    {
        boost::json::value merged = toJson(mRulesMerged);
        boost::json::value* arr = jAt(merged, arrPtr, false);
        if (!arr || !arr->is_array() || index < 0 || index >= (S32)arr->as_array().size()) return;
        boost::json::array& a = arr->as_array();
        const boost::json::value copy = a[index];   // a copy first: inserting may move the element it came from
        a.insert(a.begin() + index + 1, copy);
        rulesSet(arrPtr, arr);
        return;
    }
    stopPlacing();
    const std::string text = mDraftJson;
    if (!editDoc([&arrPtr, index, &text](boost::json::value& root)
    {
        boost::json::value* arr = jAt(root, arrPtr, false);
        if (!arr || !arr->is_array() || index < 0 || index >= (S32)arr->as_array().size()) return false;
        boost::json::value copy = arr->as_array()[index];
        std::vector<std::string> ids;
        collectIds(copy, ids);
        std::map<std::string, std::string> m;
        std::string seen = text;
        for (const std::string& id : ids)
        {
            const std::string n = uniqueId(id, seen);
            m[id] = n;
            seen += "\"id\":\"" + n + "\"";
        }
        renameIds(copy, m);
        boost::json::array& a = arr->as_array();
        a.insert(a.begin() + index + 1, copy);
        return true;
    }) && !mDraftJson.empty()) setStatus(STALE, true);
}

void WolfAdventureStudioView::askDelete(S32 doc, const std::string& ptr, const std::string& what, bool remove)
{
    // STUDIO §1 "Destructive actions ... ask inside the window, with the safe button focused"
    mDeletePtr = ptr;
    mDeleteDoc = doc;
    mConfirmKeyA = what;
    mConfirmKeyB = remove ? "remove" : "";   // [SIMPLE STUDIO 2026-10-17] page 2 says Remove / Keep, the expert pages Delete / Cancel
    ask(SC_EDIT_DELETE, 0);
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// text fields and the picker
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

void WolfAdventureStudioView::startEdit(S32 i)
{
    if (i < 0 || i >= (S32)mActs.size() || mActs[i].mPtr.empty()) return;
    const EditAct a = mActs[i];
    if (mField == F_EDIT && (a.mPtr != mEditPtr || a.mDoc != mEditDoc)) commitEdit();
    if (mField == F_EDIT && a.mPtr == mEditPtr && a.mDoc == mEditDoc) { mHits.setFocus(SH_E_TEXT, i); return; }
    mEditPtr = a.mPtr;
    mEditDoc = a.mDoc;
    mEditCap = a.mCap;
    mEditType = a.mType;
    mEditOptional = a.mOptional;
    mEditMin = a.mMin;
    mEditMax = a.mMax;
    mEditExtra = a.mExtra;
    const LLSD& v = sdAt(docRead(a.mDoc), a.mPtr);
    mEditBuf = a.mType == 'k' ? std::string() : a.mType == 'j' ? (v.isDefined() ? boost::json::serialize(toJson(v)) : std::string()) : valueText(v);
    mEditAt = 0.0;
    mField = F_EDIT;
    mHits.setFocus(SH_E_TEXT, i);
}

void WolfAdventureStudioView::commitEdit(bool enter)
{
    if (mField != F_EDIT || mEditPtr.empty()) return;
    mEditAt = 0.0;
    const LLSD& cur = sdAt(docRead(mEditDoc), mEditPtr);
    const std::string t = mEditType == 's' ? mEditBuf : trim(mEditBuf);
    // a new id and a test flag are acted on only with Enter: leaving the field drops a half-typed one
    if (!enter && (mEditType == 'k' || mEditDoc == 2)) return;
    if (mEditType == 'k')
    {
        // a new entry of an object keyed by id (RULES_PRESETS_V1.md §2 "Every collection is a JSON object keyed by id")
        if (t.empty()) return;
        if (t.size() <= 24 && std::all_of(t.begin(), t.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }))
        {
            if (sdAt(docRead(mEditDoc), mEditPtr).has(t)) { mEditError = "\"" + t + "\" is already there."; mEditErrorPtr = mEditPtr; return; }
            boost::system::error_code ec;
            const boost::json::value dv = boost::json::parse(mEditExtra, ec);
            docSet(mEditDoc, ptrJoin(mEditPtr, t), ec ? boost::json::value(boost::json::object()) : dv);
            mOpen.insert(ptrJoin(mEditPtr, t));
            mEditBuf.clear();
            mEditError.clear();
            return;
        }
        mEditError = "An id is a-z, 0-9 and _, up to 24 characters.";
        mEditErrorPtr = mEditPtr;
        return;
    }
    if (mEditDoc == 2)
    {
        // Test Play: a flag set on the test session (BRAIN_API_V1.md §5.5 test_set), typed by the flag's type
        const std::string flag = ptrLast(mEditPtr);
        if (t == valueText(cur)) return;   // unchanged: nothing is sent
        LLSD extra;
        extra["flag"] = flag;
        if (mEditType == 'i')
        {
            // adventure_pack_v1.schema.json flag min/max: -1,000,000 .. 1,000,000 (and LLSD integers are 32-bit)
            char* end = nullptr;
            errno = 0;
            const long long n = strtoll(t.c_str(), &end, 10);
            if (!end || *end != '\0' || t.empty() || errno == ERANGE || n < -1000000 || n > 1000000)
            {
                mEditError = "Type a whole number from -1000000 to 1000000.";
                mEditErrorPtr = mEditPtr;
                return;
            }
            extra["value"] = (LLSD::Integer)n;
        }
        else extra["value"] = t;
        testOp("test_set", extra, "Setting " + flag + "...");
        return;
    }
    const std::string was = mEditType == 'j' ? (cur.isDefined() ? boost::json::serialize(toJson(cur)) : std::string()) : valueText(cur);
    if (t == was && !(t.empty() && cur.isDefined() && mEditOptional))
    {
        if (mEditErrorPtr == mEditPtr) mEditError.clear();
        return;
    }
    mEditError.clear();
    mEditErrorPtr.clear();
    if (t.empty() && mEditType != 's' && !mEditOptional)
    {
        mEditError = "This needs a value.";
        mEditErrorPtr = mEditPtr;
        return;
    }
    if (t.empty() && mEditOptional)
    {
        if (cur.isDefined()) docErase(mEditDoc, mEditPtr);
        return;
    }
    switch (mEditType)
    {
    case 'i':
    case 'n':
    {
        char* end = nullptr;
        const F64 d = strtod(t.c_str(), &end);
        // strtod reads "nan" and "inf" too: JSON has neither
        if (!end || *end != '\0' || !std::isfinite(d) || (mEditType == 'i' && (d != std::floor(d) || std::fabs(d) > 9.0e15)))
        {
            mEditError = mEditType == 'i' ? "Type a whole number." : "Type a number.";
            mEditErrorPtr = mEditPtr;
            return;
        }
        if (d < mEditMin || d > mEditMax)
        {
            mEditError = "Type a number from " + valueText(LLSD(mEditMin)) + " to " + valueText(LLSD(mEditMax)) + ".";
            mEditErrorPtr = mEditPtr;
            return;
        }
        if (mEditType == 'i') docSet(mEditDoc, mEditPtr, boost::json::value((int64_t)d));
        else docSet(mEditDoc, mEditPtr, boost::json::value(d));
        break;
    }
    case 'w':
    {
        boost::json::array a;
        for (const std::string& w : splitComma(t)) { const std::string x = trim(w); if (!x.empty()) a.push_back(boost::json::value(x)); }
        docSet(mEditDoc, mEditPtr, a);
        break;
    }
    case 'j':
    {
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(t, ec);
        if (ec || v.is_null())
        {
            mEditError = "This is not valid JSON: check the brackets, quotes and commas.";
            mEditErrorPtr = mEditPtr;
            return;
        }
        docSet(mEditDoc, mEditPtr, v);
        break;
    }
    default:
        docSet(mEditDoc, mEditPtr, boost::json::value(t));
        break;
    }
}

void WolfAdventureStudioView::openPicker(const std::string& title, const std::vector<std::pair<std::string, std::string>>& opts,
                                         const std::function<void(const std::string&)>& set, const std::string& empty)
{
    commitEdit();
    mField = F_NONE;
    mPicker.mOn = true;
    mPicker.mTitle = title;
    mPicker.mOpts = opts;
    mPicker.mSet = set;
    mPicker.mEmpty = empty;
    mPickerFilter.clear();
    mPickerScroll = 0.f;
    mPickerSel = 0;
    while (mPickerSel + 1 < (S32)opts.size() && opts[mPickerSel].first == "~hdr") ++mPickerSel;
    mPickerSelMoved = true;
}

namespace
{
    /** A drawn text box (the Studio's field look, wolffloateradventurestudio.cpp field()). Returns the caret x. */
    void box(F32 l, F32 b, F32 r, F32 t, bool focused, bool invalid, const Palette& p)
    {
        chamfer(l, b, r, t, 4.f, LLColor4(0.f, 0.f, 0.f, 0.45f), LLColor4(0.f, 0.f, 0.f, 0.55f));
        chamferEdge(l + 0.5f, b + 0.5f, r - 0.5f, t - 0.5f, 4.f, focused || invalid ? 2.f : 1.f, invalid ? DANGER_INK : (focused ? p.mAccent : p.mDim));
    }
    /** An id's base word for a new item of an array, by the array's key. */
    std::string idBase(const std::string& key)
    {
        static const std::map<std::string, std::string> B = {
            { "beats", "beat" }, { "scenes", "scene" }, { "acts", "act" }, { "cast", "character" }, { "spots", "spot" }, { "areas", "area" },
            { "paths", "path" }, { "chests", "chest" }, { "triggers", "trigger" }, { "decisions", "decision" }, { "options", "option" },
            { "replies", "reply" }, { "storyCards", "card" }, { "sourceCards", "source" }, { "encounters", "encounter" }, { "parcels", "reward" },
            { "handouts", "handout" }, { "quizzes", "quiz" }, { "items", "item" }, { "choices", "choice" }, { "endings", "ending" },
            { "flags", "flag" }, { "factions", "faction" }, { "bands", "band" }, { "enemies", "enemy" }, { "battles", "battle" } };
        auto it = B.find(key);
        return it != B.end() ? it->second : (key.empty() ? std::string("item") : key);
    }

    /** The words after "Add " on a list's add button: plain words where the list's label would read oddly
     *  ("Add Secrets - never revealed" -> "Add a secret"), else the list's singular (idBase). [TIDY FORM 2026-10-17] */
    std::string addWordsFor(const std::string& key)
    {
        static const std::map<std::string, std::string> A = { { "neverReveal", "a secret" }, { "neverSay", "a line they never say" }, { "idle", "an idle line" } };
        auto it = A.find(key);
        return it != A.end() ? it->second : labelFor(idBase(key));
    }
}

/** A row: the label in the left column (wrapped); `cl` gets the control's left edge. */
F32 WolfAdventureStudioView::fRow(F32 l, F32 r, F32 top, const std::string& label, F32& cl)
{
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const F32 lw = llmin(LABEL_W, (r - l) * 0.4f);
    const std::vector<std::string> lines = wrap(small, label, lw - 6.f, 2);
    F32 y = top - 6.f;
    for (const std::string& line : lines) { textL(small, line, l, y, gamePalette().mText); y -= (F32)small->getLineHeight(); }
    cl = l + lw;
    return top;
}

/** The error the brain reports at exactly this pointer (an error first, else a warning), or "". */
std::string WolfAdventureStudioView::problemAt(const std::string& ptr) const
{
    const auto it = ptr.empty() ? mProblemText.end() : mProblemText.find(ptr);
    return it == mProblemText.end() ? std::string() : it->second.second;
}

/** How many of the brain's problems are at or under this pointer (the Outline's per-node count, §2.2; indexed in setProblems). */
S32 WolfAdventureStudioView::problemsUnder(const std::string& ptr) const
{
    const auto it = mProblemCount.find(ptr);
    return it == mProblemCount.end() ? 0 : it->second;
}

F32 WolfAdventureStudioView::fText(F32 l, F32 r, F32 top, const std::string& label, const std::string& ptr, const LLSD& v, S32 doc, size_t capn,
                                   char type, bool optional, const std::string& tip, const Palette& p, F64 mn, F64 mx)
{
    const LLFontGL* f = LLFontGL::getFontSansSerif();
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const F32 lh = (F32)f->getLineHeight();
    F32 cl = l;
    F32 ctop = top;   // the box's top: under the label when labels sit above (mLabelsAbove), else `top` beside it
    if (!label.empty())
    {
        if (mLabelsAbove)
        {
            // [TIDY FORM 2026-10-17] the label above the box, left-aligned; the box takes the whole width (one column of boxes)
            const LLFontGL* lb = LLFontGL::getFontSansSerifSmallBold();
            textL(lb, label, l, top, p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l);
            ctop = top - (F32)lb->getLineHeight() - 3.f;
        }
        else fRow(l, r, top, label, cl);
    }
    const S32 idx = textAct(ptr, doc, capn, type, optional, mn, mx);
    const bool editing = mField == F_EDIT && mEditPtr == ptr && mEditDoc == doc;
    const bool focused = editing && hasFocus();
    std::string text = editing ? mEditBuf : (type == 'j' ? (v.isMap() ? boost::json::serialize(toJson(v)) : std::string()) : valueText(v));
    const bool multi = type == 'j' || (type == 's' && capn > 240);
    const F32 w = r - cl;
    std::vector<std::string> lines;
    F32 h = ROW_H;
    if (multi)
    {
        lines = wrap(f, text, w - 16.f, 0);
        h = llmax(3.f, (F32)lines.size()) * lh + 10.f;
    }
    const F32 b = ctop - h;
    const std::string err = mEditErrorPtr == ptr && !mEditError.empty() ? mEditError : problemAt(ptr);
    const bool invalid = !err.empty();
    box(cl, b, r, ctop, focused, invalid, p);
    F32 caret_x = cl + 8.f, caret_y = (b + ctop) * 0.5f;
    if (multi)
    {
        F32 y = ctop - 5.f;
        if (text.empty()) textL(f, optional ? "(empty)" : "Type here", cl + 8.f, y, p.mDim);
        for (const std::string& line : lines)
        {
            caret_x = textL(f, line, cl + 8.f, y, p.mText);
            caret_y = y - lh * 0.5f;
            y -= lh;
        }
    }
    else
    {
        if (text.empty())
        {
            const std::string ph = type == 'i' || type == 'n' ? std::string(optional ? "(not set)" : "a number")
                                 : type == 'w' ? std::string("words, separated by commas") : std::string(optional ? "(empty)" : "Type here");
            textL(f, ph, cl + 8.f, caret_y, p.mDim, LLFontGL::LEFT, LLFontGL::VCENTER, w - 16.f);
        }
        else caret_x = textL(f, text, cl + 8.f, caret_y, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, w - 16.f);
    }
    if (focused && fmod(now(), 1.0) < 0.6) rect(caret_x + 1.f, caret_y - lh * 0.45f, caret_x + 2.f, caret_y + lh * 0.45f, p.mText);
    if (mKeyboardNav && hasFocus() && mHits.isFocused(SH_E_TEXT, idx) && !editing) focusRing(cl, b, r, ctop, p);
    std::string full_tip = tip.empty() ? label : tip;
    if (capn > 0 && (type == 's' || type == 'w')) full_tip += llformat(" (up to %d characters)", (S32)capn);
    if ((type == 'i' || type == 'n') && mn > -1e17 && mx < 1e17) full_tip += " (" + valueText(LLSD(mn)) + " to " + valueText(LLSD(mx)) + ")";
    hit(cl, b, r, ctop, SH_E_TEXT, idx, full_tip);
    F32 y = b - 2.f;
    // the length counter (STUDIO §2.3 "persona (counter: 2000)", §2.5 "a counter of 240")
    if (capn > 0 && (type == 's') && (multi || focused))
    {
        const S32 used = (S32)utf8str_to_wstring(text).size();
        textL(small, llformat("%d of %d", used, (S32)capn), r, y, used >= (S32)capn ? WARN_INK : p.mDim, LLFontGL::RIGHT, LLFontGL::TOP);
        y -= (F32)small->getLineHeight();
    }
    if (invalid)
    {
        // §6 3.3.1: the field gets the error mark (a red rim) AND the message beside it
        textL(LLFontGL::getFontSansSerifSmallBold(), "Problem:", cl, y, DANGER_INK);
        y = para(cl + 60.f, r, y, err, DANGER_INK, true);
    }
    // a problem's field: brought into view and focused (§2.2 "Clicking one opens its JSON pointer's editor and focuses the field")
    if (mFocusScroll && focusWants(ptr))
    {
        mScroll[mPage] = llmax(0.f, mScroll[mPage] + ((F32)mPageRect.mTop - 40.f - top));
        mFocusScroll = false;
        mKeyboardNav = true;
        startEdit(idx);
    }
    return y - 6.f;
}

F32 WolfAdventureStudioView::fBool(F32 l, F32 r, F32 top, const std::string& label, const std::string& ptr, const LLSD& v, bool dflt, S32 doc,
                                   const Palette& p, const std::string& tip)
{
    F32 cl = l;
    fRow(l, r, top, label, cl);
    const bool on = v.isBoolean() ? v.asBoolean() : dflt;
    const S32 idx = act([this, doc, ptr, on]() { docSet(doc, ptr, boost::json::value(!on)); });
    // the state as a word as well as the lit button (colour never alone)
    btn(cl, top - ROW_H, cl + 90.f, top, on ? "Yes" : "No", SH_E, idx, (tip.empty() ? label : tip) + (on ? " - now Yes; press for No" : " - now No; press for Yes"), p, true, on ? BTN_ACTIVE : 0);
    if (!v.isDefined()) textL(LLFontGL::getFontSansSerifSmall(), "(default)", cl + 98.f, top - ROW_H * 0.5f, p.mDim, LLFontGL::LEFT, LLFontGL::VCENTER);
    const std::string err = problemAt(ptr);
    F32 y = top - ROW_H - 2.f;
    if (!err.empty()) y = para(cl, r, y, err, DANGER_INK, true);
    return y - 6.f;
}

F32 WolfAdventureStudioView::fEnum(F32 l, F32 r, F32 top, const std::string& label, const std::string& ptr, const std::vector<std::string>& opts,
                                   const LLSD& v, const std::string& dflt, S32 doc, bool optional, const Palette& p)
{
    F32 cl = l;
    fRow(l, r, top, label, cl);
    const std::string cur = v.isString() ? v.asString() : std::string();
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    // segmented buttons when they fit on one line, else a picker
    F32 need = 0.f;
    for (const std::string& o : opts) need += btnW(labelFor(o)) + 4.f;
    F32 y = top;
    if (need <= r - cl && opts.size() <= 6)
    {
        F32 x = cl;
        for (const std::string& o : opts)
        {
            const F32 bw = btnW(labelFor(o));
            const bool on = cur == o || (cur.empty() && o == dflt);
            const S32 idx = act([this, doc, ptr, o]() { docSet(doc, ptr, boost::json::value(o)); });
            btn(x, top - ROW_H, x + bw, top, labelFor(o), SH_E, idx, label + ": " + labelFor(o) + (on ? " (chosen)" : ""), p, true, on ? BTN_ACTIVE : 0);
            x += bw + 4.f;
        }
        y = top - ROW_H;
    }
    else
    {
        std::vector<std::pair<std::string, std::string>> po;
        for (const std::string& o : opts) po.push_back({ o, labelFor(o) });
        const S32 idx = act([this, doc, ptr, po, label, optional]()
        {
            std::vector<std::pair<std::string, std::string>> all = po;
            if (optional) all.insert(all.begin(), { std::string(), "(not set)" });
            openPicker(label, all, [this, doc, ptr](const std::string& val)
            {
                if (val.empty()) docErase(doc, ptr); else docSet(doc, ptr, boost::json::value(val));
            }, std::string());
        });
        btn(cl, top - ROW_H, r, top, cur.empty() ? (dflt.empty() ? std::string("Choose...") : labelFor(dflt) + " (default)") : labelFor(cur), SH_E, idx,
            "Choose " + label, p);
        y = top - ROW_H;
    }
    if (!v.isDefined() && !dflt.empty() && need <= r - cl) textL(small, "(default)", r, y - 2.f, p.mDim, LLFontGL::RIGHT, LLFontGL::TOP);
    const std::string err = problemAt(ptr);
    y -= 2.f;
    if (!err.empty()) y = para(cl, r, y, err, DANGER_INK, true);
    return y - 6.f;
}

std::vector<std::pair<std::string, std::string>> WolfAdventureStudioView::refOptions(const std::string& kind, const std::string& ptr) const
{
    // made once per draft change (fRef asks every frame to show a choice's words): the lists that depend on
    // where the field is (a beat's decisions, a decision's options, quick replies) are kept per parent pointer
    if (mRefCacheGen != mDraftGen) { mRefCache.clear(); mRefCacheGen = mDraftGen; }
    const bool local = kind == "decision" || kind == "option" || kind == "reply";
    const std::string key = local ? kind + "\n" + ptrParent(ptr) : kind;
    auto it = mRefCache.find(key);
    if (it == mRefCache.end()) it = mRefCache.emplace(key, refOptionsMake(kind, ptr)).first;
    return it->second;
}

std::vector<std::pair<std::string, std::string>> WolfAdventureStudioView::refOptionsMake(const std::string& kind, const std::string& ptr) const
{
    std::vector<std::pair<std::string, std::string>> out;
    const LLSD& pk = mDraftPack;
    auto list = [&](const LLSD& arr, const char* label_key)
    {
        for (const LLSD& x : llsd::inArray(arr))
        {
            const std::string id = x["id"].asString();
            if (id.empty()) continue;
            const std::string lb = label_key ? cap(x[label_key].asString(), 80) : std::string();
            out.push_back({ id, lb.empty() ? id : lb + " (" + id + ")" });
        }
    };
    if (kind == "cast")
    {
        for (const LLSD& c : llsd::inArray(pk["cast"]))
        {
            // [WOLF ADVENTURE AUDIT 2026-10-17] the wizard's placeholder says plainly that it is waiting for a name
            const bool placeholder = c["first"].asString() == "New" && c["last"].asString() == "Character";
            out.push_back({ c["id"].asString(), placeholder ? std::string("New Character (not named yet - name them in step 4 or on the Cast page)")
                                                            : cap(c["first"].asString() + " " + c["last"].asString(), 64) + " (" + c["id"].asString() + ")" });
        }
    }
    else if (kind == "spot") list(pk["places"]["spots"], "label");
    else if (kind == "area") list(pk["places"]["areas"], "label");
    else if (kind == "path") list(pk["places"]["paths"], "label");
    else if (kind == "chest") list(pk["places"]["chests"], "label");
    else if (kind == "beat" || kind == "visited" || kind == "scene")
    {
        for (const LLSD& a : llsd::inArray(pk["acts"]))
            for (const LLSD& sc : llsd::inArray(a["scenes"]))
            {
                if (kind != "beat") out.push_back({ sc["id"].asString(), "Scene: " + cap(sc["title"].asString(), 60) + " (" + sc["id"].asString() + ")" });
                if (kind != "scene")
                    for (const LLSD& b : llsd::inArray(sc["beats"]))
                        out.push_back({ b["id"].asString(), cap(sc["title"].asString(), 30) + " - " + cap(b["objective"].asString(), 60) + " (" + b["id"].asString() + ")" });
            }
    }
    else if (kind == "decision" || kind == "option" || kind == "reply")
    {
        // the beat the pointer is inside (its decisions; a decision's options; a character's quick replies there)
        const std::string beat = ptrUpTo(ptr, "beats", 0);
        const LLSD& b = beat.empty() ? LLSD() : sdAt(pk, beat);
        const LLSD& parent = sdAt(pk, ptrParent(ptr));
        if (kind == "decision")
        {
            if (b.isMap()) list(b["decisions"], "prompt");
            else
                for (const LLSD& a : llsd::inArray(pk["acts"])) for (const LLSD& sc : llsd::inArray(a["scenes"])) for (const LLSD& bb : llsd::inArray(sc["beats"])) list(bb["decisions"], "prompt");
        }
        else if (kind == "option")
        {
            const std::string dec = parent["decision"].asString();
            for (const LLSD& d : llsd::inArray(b["decisions"])) if (d["id"].asString() == dec) list(d["options"], "label");
        }
        else
        {
            const std::string c = parent["cast"].asString();
            for (const LLSD& od : llsd::inArray(b["onDuty"])) if (od["cast"].asString() == c) list(od["replies"], "label");
        }
    }
    else if (kind == "parcel") list(pk["parcels"], "label");
    else if (kind == "enemy") list(pk["enemies"], "name");     // [WOLF ADVENTURE COMBAT 2026-10-17]
    else if (kind == "battle") list(pk["battles"], "label");
    else if (kind == "ending") list(pk["endings"], "title");
    else if (kind == "handout") list(pk["handouts"], "title");
    else if (kind == "quiz") list(pk["quizzes"], "title");
    else if (kind == "encounter") list(pk["encounters"], "label");
    else if (kind == "flag") list(pk["state"]["flags"], "journal");
    else if (kind == "faction") list(pk["state"]["factions"], "name");
    else if (kind == "storyCard") list(pk["storyCards"], "title");
    else if (kind == "sourceCard")
    {
        for (const LLSD& c : llsd::inArray(pk["sourceCards"])) out.push_back({ c["id"].asString(), cap(c["citation"]["title"].asString(), 60) + " (" + c["id"].asString() + ")" });
    }
    else if (kind == "band")
    {
        // the bands of the faction named beside it, else honour's
        const LLSD& parent = sdAt(pk, ptrParent(ptr));
        const std::string fac = parent["rep"].asString();
        const LLSD* bands = &pk["state"]["honour"]["bands"];
        for (const LLSD& fc : llsd::inArray(pk["state"]["factions"])) if (!fac.empty() && fc["id"].asString() == fac) bands = &fc["bands"];
        if (parent.has("castRep") || (fac.empty() && !parent.has("honour")))
        {
            // HUD §3.5: a character's opinion uses the design-choice bands
            for (const char* bnd : { "hostile", "wary", "neutral", "friendly", "trusted" }) out.push_back({ bnd, labelFor(bnd) });
        }
        else list(*bands, "label");
    }
    else if (kind == "check")
    {
        for (const auto& kv : llsd::inMap(std::as_const(mRulesMerged)["checks"])) out.push_back({ kv.first, cap(kv.second["label"].asString(), 60) + " (" + kv.first + ")" });
    }
    else if (kind == "statBlock")
    {
        for (const auto& kv : llsd::inMap(std::as_const(mRulesMerged)["data"]["monsters"])) out.push_back({ kv.first, cap(kv.second["name"].asString(), 60) + " (" + kv.first + ")" });
    }
    else if (kind == "aiTrigger")
    {
        // the `ai` triggers that name this character (ADVENTURE_PACK_V1.md §7 triggerIds; §11.1 `ai` needs cast)
        const std::string castPtr = ptrUpTo(ptr, "cast", 0);
        const std::string cid = sdAt(pk, castPtr)["id"].asString();
        for (const LLSD& a : llsd::inArray(pk["acts"])) for (const LLSD& sc : llsd::inArray(a["scenes"])) for (const LLSD& b : llsd::inArray(sc["beats"]))
            for (const LLSD& t : llsd::inArray(b["triggers"]))
                if (t["on"].asString() == "ai" && t["cast"].asString() == cid)
                    out.push_back({ t["id"].asString(), cap(b["objective"].asString(), 60) + " (" + t["id"].asString() + ")" });
    }
    else if (kind == "choice")
    {
        const LLSD& item = sdAt(pk, ptrParent(ptr));
        list(item["choices"], "label");
    }
    else if (kind == "region")
    {
        for (const LLSD& rg : llsd::inArray(pk["adventure"]["regions"])) out.push_back({ rg["region"].asString(), cap(rg["name"].asString(), 64) });
    }
    return out;
}

void WolfAdventureStudioView::pickRef(const std::string& title, const std::string& ptr, const std::string& kind, S32 doc, bool optional)
{
    std::vector<std::pair<std::string, std::string>> opts = refOptions(kind, ptr);
    if (optional) opts.insert(opts.begin(), { std::string(), "(none)" });
    static const std::map<std::string, std::string> WHERE = {
        { "cast", "Add characters on the Cast page first." }, { "spot", "Add spots on the Places page first." },
        { "area", "Add areas on the Places page first." }, { "path", "Add paths on the Places page first." },
        { "chest", "Add chests on the Places page first." }, { "beat", "Add beats on the Outline page first." },
        { "scene", "Add scenes on the Outline page first." }, { "decision", "Add a decision to this beat first." },
        { "option", "Choose the decision first, and give it options." }, { "reply", "Give the character quick replies on this beat first." },
        { "parcel", "Add rewards on the Lore page first." }, { "ending", "Add endings on the Outline page first." },
        { "handout", "Add handouts on the Lore page first." }, { "quiz", "Add quizzes on the Lore page first." },
        { "encounter", "Add encounters on the Lore page first." }, { "flag", "Add flags on the Rules page first." },
        { "faction", "Add factions on the Rules page first." }, { "storyCard", "Add story cards on the Lore page first." },
        { "sourceCard", "Add source cards on the Lore page first." }, { "check", "This rules preset has no checks." },
        { "statBlock", "This rules preset has no stat blocks yet." }, { "aiTrigger", "Add a 'When the character decides (AI)' trigger naming this character first." },
        { "choice", "Give this item choices first." }, { "region", "Add regions to the adventure first (Outline)." },
        { "enemy", "Add enemies on the Lore page (Enemies) first." }, { "battle", "Add battles on the Lore page (Battles) first." } };
    auto w = WHERE.find(kind);
    openPicker(title, opts, [this, doc, ptr](const std::string& val)
    {
        if (val.empty()) docErase(doc, ptr); else docSet(doc, ptr, boost::json::value(val));
    }, w != WHERE.end() ? w->second : std::string("Nothing to choose from yet."));
}

F32 WolfAdventureStudioView::fRef(F32 l, F32 r, F32 top, const std::string& label, const std::string& ptr, const std::string& kind, const LLSD& v,
                                  S32 doc, bool optional, const Palette& p)
{
    F32 cl = l;
    fRow(l, r, top, label, cl);
    const std::string cur = v.asString();
    std::string shown = cur.empty() ? std::string(optional ? "(none)" : "Choose...") : cur;
    bool missing = false;
    if (!cur.empty())
    {
        missing = true;
        for (const auto& o : refOptions(kind, ptr)) if (o.first == cur) { shown = o.second; missing = false; }
    }
    const S32 idx = act([this, label, ptr, kind, doc, optional]() { pickRef(label, ptr, kind, doc, optional); });
    btn(cl, top - ROW_H, r, top, missing ? cur + " (not in the pack)" : shown, SH_E, idx, "Choose " + label, p, true, missing ? BTN_DANGER : 0);
    F32 y = top - ROW_H - 2.f;
    const std::string err = problemAt(ptr);
    if (!err.empty()) y = para(cl, r, y, err, DANGER_INK, true);
    if (mFocusScroll && focusWants(ptr))
    {
        mScroll[mPage] = llmax(0.f, mScroll[mPage] + ((F32)mPageRect.mTop - 40.f - top));
        mFocusScroll = false;
        mKeyboardNav = true;
        mHits.setFocus(SH_E, idx);
    }
    return y - 6.f;
}

/**
 * A card's head for an item of a list: its title, then Open/Close, Move up, Move down, Duplicate and Delete
 * (STUDIO §2.2 "add, duplicate and delete at any level; reorder by ... Move up / Move down").
 */
F32 WolfAdventureStudioView::cardHead(F32 l, F32 r, F32 top, const std::string& ptr, const std::string& title, S32 index, S32 count,
                                      const std::string& arrPtr, bool can_delete, const Palette& p, bool open_toggle, S32 doc)
{
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 h = 28.f;
    const bool open = mOpen.count(ptr) > 0;
    rect(l, top - h, r, top, fade(p.mCard, 0.9f));
    F32 x = r;
    auto small_btn = [&](const std::string& lab, const std::string& tip, const std::function<void()>& fn, bool enabled, U32 extra = 0)
    {
        const F32 bw = btnW(lab);
        x -= bw + 3.f;
        const S32 idx = act(fn);
        btn(x, top - h + 2.f, x + bw, top - 2.f, lab, SH_E, idx, tip, p, enabled, extra);
    };
    small_btn("Delete", "Delete " + title + " (asks first)", [this, ptr, title, doc]() { askDelete(doc, ptr, title); }, can_delete, BTN_DANGER);
    small_btn("Duplicate", "Duplicate " + title, [this, arrPtr, index, doc]() { docDuplicate(doc, arrPtr, index); }, true);
    small_btn("Down", "Move " + title + " down", [this, arrPtr, index, doc]() { docMove(doc, arrPtr, index, 1); }, index + 1 < count);
    small_btn("Up", "Move " + title + " up", [this, arrPtr, index, doc]() { docMove(doc, arrPtr, index, -1); }, index > 0);
    F32 tl = l + 6.f;
    if (open_toggle)
    {
        const S32 idx = act([this, ptr, open]() { if (open) mOpen.erase(ptr); else mOpen.insert(ptr); });
        chevron(l + 12.f, top - h * 0.5f, open, p.mText);
        hit(l, top - h, x - 4.f, top, SH_E, idx, (open ? "Close " : "Open ") + title);
        if (mKeyboardNav && hasFocus() && mHits.isFocused(SH_E, idx)) focusRing(l, top - h, x - 4.f, top, p);
        tl = l + 24.f;
    }
    const S32 n = problemsUnder(ptr);
    F32 te = x - 6.f;
    if (n > 0)
    {
        // an icon AND a number (§2.2 "its problem count (an icon and a number)")
        const std::string pw = llformat("%d", n);
        const F32 pw_w = (F32)bold->getWidthF32(pw) + 18.f;
        te -= pw_w;
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.begin(LLRender::TRIANGLES);
        gGL.color4fv(WARN_INK.mV);
        const F32 cy = top - h * 0.5f;
        gGL.vertex2f(te + 2.f, cy - 5.f); gGL.vertex2f(te + 12.f, cy - 5.f); gGL.vertex2f(te + 7.f, cy + 5.f);
        gGL.end();
        textL(bold, pw, te + 15.f, cy, WARN_INK, LLFontGL::LEFT, LLFontGL::VCENTER);
    }
    textL(bold, title, tl, top - h * 0.5f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, te - tl - 4.f);
    return top - h - 4.f;
}

namespace
{
    /**
     * [WOLF ADVENTURE AUDIT 2026-10-17] The starter words a new item's required text gets, so nothing new starts in error
     * (Paul 2026-10-17: Add act / scene / beat made "must be text" problems). Which members are required text comes
     * from adventure_pack_v1.schema.json (act/scene/handout/ending title, beat objective, castSheet role, decision and
     * quiz-item prompt, story/source card, ending, hint, line and journal text, choice label, credit/faction name).
     * `def` is the $defs name of the item being made, `index` its place in its list (-1: not known).
     */
    std::string starterText(const std::string& def, const std::string& parent_key, const std::string& k, S32 index)
    {
        const S32 n = index >= 0 ? index + 1 : 1;
        if (k == "title")
        {
            if (def == "act") return llformat("Act %d", n);
            if (def == "scene") return llformat("Scene %d", n);
            if (def == "ending") return n == 1 ? std::string("The end") : llformat("Ending %d", n);
            if (def == "handout") return llformat("Handout %d", n);
            if (parent_key == "citation") return "The source's title";
            return llformat("Untitled %d", n);
        }
        if (k == "objective") return "Describe what the players must do here";
        if (k == "role") return parent_key == "credits" ? std::string("Writer") : std::string("Describe who they are");
        if (k == "prompt") return def == "decision" ? std::string("What do the players choose?") : std::string("Type the question here");
        if (k == "label") return llformat("Choice %d", n);
        if (k == "name") return parent_key == "factions" ? llformat("Faction %d", n) : std::string("Your name");
        if (k == "work") return "The work's title";
        if (k == "text")
        {
            if (def == "ending") return "Write what the players read when the story ends here.";
            if (def == "storyCard") return "What the characters know about this.";
            if (def == "sourceCard") return "The fact, as the source gives it.";
            if (parent_key == "hints") return "A hint that helps the players on.";
            if (def == "line" || parent_key == "idle" || parent_key == "lines" || parent_key == "line") return "Write the line here.";
            if (parent_key == "journal") return "Write the journal entry here.";
            return "Write the text here.";
        }
        if (k == "item" && parent_key != "take") return "Item name";
        return std::string();
    }

    /** A new value for a schema, with its required members filled (ids unique within the pack). `index` is the new
     *  item's place in its list (for starter words like "Act 2"); -1 when not known. */
    boost::json::value makeDefault(const boost::json::value& root, const boost::json::value& s0, const std::string& key, const std::string& packText,
                                   const std::string& region, S32 index = -1)
    {
        std::string name;
        const boost::json::value& s = resolve(root, s0, &name);
        if (name == "condition") return boost::json::object{ { "flag", "" } };
        if (name == "vec3") return boost::json::array{ -1, -1, 0 };
        if (name == "vec2") return boost::json::array{ -1, -1 };
        const std::vector<std::string> en = sEnum(s);
        if (!en.empty())
        {
            const LLSD d = sDefault(s);
            return boost::json::value(d.isString() ? d.asString() : en.front());
        }
        // [WOLF ADVENTURE AUDIT 2026-10-17] a number enum (dice sides): its default, else its first member
        const std::vector<S64> enn = sEnumNums(s);
        if (!enn.empty())
        {
            const LLSD d = sDefault(s);
            return boost::json::value((int64_t)(d.isInteger() ? (S64)d.asInteger() : enn.front()));
        }
        const std::string t = sType(s);
        if (name == "id") return boost::json::value(key == "id" ? uniqueId(idBase(key), packText) : std::string());
        if (name == "uuid") return boost::json::value(key == "region" ? region : std::string());
        if (t == "boolean") { const LLSD d = sDefault(s); return boost::json::value(d.isBoolean() ? d.asBoolean() : false); }
        if (t == "integer" || t == "number")
        {
            F64 d = 0.0;
            const LLSD dv = sDefault(s);
            if (dv.isDefined()) d = dv.asReal();
            else if (sNum(s, "minimum", d)) {}
            else if (sNum(s, "exclusiveMinimum", d)) d += 1.0;
            return t == "integer" ? boost::json::value((int64_t)d) : boost::json::value(d);
        }
        if (t == "array")
        {
            boost::json::array a;
            F64 mn = 0.0;
            if (sNum(s, "minItems", mn) && mn >= 1.0)
            {
                const boost::json::value* items = s.as_object().if_contains("items");
                if (items) for (S32 i = 0; i < (S32)mn; ++i) a.push_back(makeDefault(root, *items, key, packText, region));
            }
            return a;
        }
        if (isObjectSchema(s, t))
        {
            boost::json::object o;
            if (s.is_object() && s.as_object().if_contains("oneOf"))
            {
                // an area's shape: a circle to start with
                o["circle"] = boost::json::object{ { "centre", boost::json::array{ -1, -1 } }, { "radius", 10 } };
                return o;
            }
            for (const std::string& k : propKeys(s))
            {
                if (!isRequired(s, k)) continue;
                const boost::json::value& ps = prop(s, k);
                std::string pn;
                const boost::json::value& rs = resolve(root, ps, &pn);
                if (k == "id") o[k] = uniqueId(idBase(key), packText);
                else if (k == "first") o[k] = "New";
                else if (k == "last") o[k] = "Character";
                else if (sType(rs) == "string" && sEnum(rs).empty() && pn != "id" && pn != "uuid" && pn != "uuidOrEmpty" && pn != "anim")
                {
                    // required text starts with words that say what to write (never empty, never in error)
                    std::string w = starterText(name, key, k, index);
                    F64 maxlen = 0.0;
                    if (sNum(rs, "maxLength", maxlen) && maxlen > 0.0) w = WolfGame::cleanText(w, (size_t)maxlen);
                    if (!w.empty() && sStr(rs, "pattern").empty()) o[k] = w;
                    else o[k] = makeDefault(root, ps, k, packText, region);
                }
                else o[k] = makeDefault(root, ps, k, packText, region);
            }
            return o;
        }
        return boost::json::value(std::string());
    }
}

bool WolfAdventureStudioView::focusWants(const std::string& ptr) const
{
    return !mFocusPtr.empty() && !ptr.empty() && (mFocusPtr == ptr || ptr.rfind(mFocusPtr + "/", 0) == 0);
}

S32 WolfAdventureStudioView::repairLegacyDraft()
{
    if (mDraftJson.empty() || !openIsOwner()) return 0;
    if (!mDraftDoc.is_object())
    {
        boost::system::error_code ec;
        mDraftDoc = boost::json::parse(mDraftJson, ec);
        if (ec || !mDraftDoc.is_object()) { mDraftDoc = nullptr; return 0; }
    }
    const boost::json::value& root = packSchema();
    S32 fixed = 0;
    // the pack walked with its schema: an object's members by their property schemas, a list's items by `items`
    std::function<void(boost::json::value&, const boost::json::value&, const std::string&, S32)> walk =
        [&](boost::json::value& v, const boost::json::value& s0, const std::string& key, S32 index)
    {
        std::string name;
        const boost::json::value& s = resolve(root, s0, &name);
        if (!s.is_object()) return;
        if (v.is_array())
        {
            const boost::json::value* items = s.as_object().if_contains("items");
            if (!items) return;
            S32 i = 0;
            for (auto& x : v.as_array()) walk(x, *items, key, i++);
            return;
        }
        if (!v.is_object() || !isObjectSchema(s, sType(s))) return;
        boost::json::object& o = v.as_object();
        for (const std::string& k : propKeys(s))
        {
            boost::json::value* m = o.if_contains(k);
            if (!m) continue;
            const boost::json::value& ps = prop(s, k);
            std::string pn;
            const boost::json::value& rs = resolve(root, ps, &pn);
            const bool text = sType(rs) == "string" && sEnum(rs).empty() && sStr(rs, "pattern").empty() && pn != "id" && pn != "uuid"
                           && pn != "uuidOrEmpty" && pn != "anim" && pn != "colour";
            if (text && !m->is_string() && !m->is_number() && !m->is_bool())
            {
                // what the old makeDefault wrote: {} (or a list / null) - starter words, cut to the field's length
                std::string w = starterText(name, key, k, index);
                if (w.empty()) w = "Write the text here.";
                F64 maxlen = 0.0;
                if (sNum(rs, "maxLength", maxlen) && maxlen > 0.0) w = WolfGame::cleanText(w, (size_t)maxlen);
                *m = w;
                ++fixed;
                continue;
            }
            if (m->is_object() || m->is_array()) walk(*m, ps, k, -1);
        }
    };
    walk(mDraftDoc, root, std::string(), -1);
    if (fixed == 0) return 0;
    mDraftJson = boost::json::serialize(mDraftDoc);
    mDraftPack = LlsdFromJson(mDraftDoc);
    ++mDraftGen;
    mDirtyAt = now();   // saved by the autosave (STUDIO §1, 2 s), with the draft's etag as any edit
    return fixed;
}

F32 WolfAdventureStudioView::fValue(F32 l, F32 r, F32 top, const std::string& ptr, const std::string& key, const boost::json::value& schema0,
                                    const LLSD& v, bool optional, S32 doc, const Palette& p, const std::string& label_in)
{
    const boost::json::value& root = doc == 1 ? rulesSchema() : packSchema();
    std::string name;
    const boost::json::value& s = resolve(root, schema0, &name);
    std::string label = label_in.empty() ? labelFor(key) : label_in;
    const std::string tip = sDescription(s).empty() ? sDescription(schema0) : sDescription(s);
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    // the rules editor: a value the pack changed from its preset shows a dot AND the word (STUDIO §2.6)
    if (doc == 1 && !v.isMap() && rulesOverridden(ptr))
    {
        disc(l - 6.f, top - 12.f, 3.f, WARN_INK);
        label += " - changed";
    }
    // a texture: preview, drop target and the viewer's texture picker (§2.9)
    // [WOLF ADVENTURE AUDIT 2026-10-17] the adventure's cover and logo are textures too (adventure.cover, adventure.look.logo;
    // a title card's picture is look.titleCard.image) - never a bare 36-character box for a beginner
    if (doc == 0 && name == "uuidOrEmpty" && (key == "texture" || key == "portrait" || key == "image" || key == "cover" || key == "logo"))
        return fTexture(l, r, top, label, ptr, v, p);
    // [WOLF ADVENTURE AUDIT 2026-10-17] a colour (adventure.look accent / panel, reputation bands): a swatch beside the box
    if (name == "colour") return fColour(l, r, top, label, ptr, v, doc, optional, p);
    if (doc == 0 && name == "condition")
    {
        if (!v.isDefined())
        {
            const S32 idx = act([this, ptr]() { docSet(0, ptr, boost::json::object{ { "flag", "" } }); });
            F32 cl = l;
            fRow(l, r, top, label, cl);
            btn(cl, top - ROW_H, cl + 120.f, top, "Add a rule", SH_E, idx, "Add an '" + label + "' condition", p);
            return top - ROW_H - 8.f;
        }
        F32 y = head(l, r, top, label, p);
        y = fCondition(l + 10.f, r, y, ptr, v, 0, p, 1);
        if (optional)
        {
            const S32 idx = act([this, ptr]() { docErase(0, ptr); });
            btn(l + 10.f, y - ROW_H, l + 170.f, y, "Remove the rule", SH_E, idx, "Remove '" + label + "'", p, true, BTN_DANGER);
            y -= ROW_H + 6.f;
        }
        return y;
    }
    if (doc == 0 && name == "effects") return fEffects(l, r, top, ptr, v, p, label);
    if (key == "statBlock")
    {
        // 5E: a stat block id from the preset's data; other presets: a stat map (adventure_pack_v1.schema.json actor.statBlock oneOf)
        if (mPresetId == "5e-srd-5.2") return fRef(l, r, top, label, ptr, "statBlock", v, doc, true, p);
        return fText(l, r, top, label + " (stat map, JSON)", ptr, v, doc, 0, 'j', true, "e.g. {\"grit\": 3, \"draw\": 2}", p);
    }
    if (name == "formula") return fText(l, r, top, label + " (formula, JSON)", ptr, v, doc, 0, 'j', true, "an integer, or one operator object (RULES_PRESETS_V1.md §5)", p);
    if (doc == 0 && key == "triggers") return fTriggers(l, r, top, ptr, v, p);
    if (name == "vec3" || name == "vec2")
    {
        const S32 n = name == "vec3" ? 3 : 2;
        F32 cl = l;
        fRow(l, r, top, label, cl);
        if (!v.isArray() || (S32)v.size() != n)
        {
            const S32 idx = act([this, doc, ptr, n]() { docSet(doc, ptr, n == 3 ? boost::json::array{ -1, -1, 0 } : boost::json::array{ -1, -1 }); });
            btn(cl, top - ROW_H, cl + 120.f, top, "Set", SH_E, idx, "Set " + label, p);
            return top - ROW_H - 8.f;
        }
        const F32 w = (r - cl - 6.f * (n - 1)) / n;
        F32 lowest = top;
        static const char* AX[3] = { "x", "y", "z" };
        for (S32 i = 0; i < n; ++i)
        {
            const F32 x0 = cl + i * (w + 6.f);
            const F32 y = fText(x0, x0 + w, top, std::string(), ptrIndex(ptr, i), v[i], doc, 0, 'n', false, label + " " + AX[i], p, -10000.0, 10000.0);
            lowest = llmin(lowest, y);
        }
        const std::string err = problemAt(ptr);
        if (!err.empty()) lowest = para(cl, r, lowest, err, DANGER_INK, true) - 4.f;
        if (v[0].asReal() < 0.0 || v[1].asReal() < 0.0)
        {
            // WOLF_MIND_V1.md §10.2 step 4: unplaced (from a book draft); STUDIO §2.4 "an unplaced badge"
            textL(LLFontGL::getFontSansSerifSmallBold(), "Unplaced - place it in the world on the Places page", cl, lowest, WARN_INK);
            lowest -= (F32)small->getLineHeight() + 4.f;
        }
        return lowest;
    }
    if (s.is_object() && s.as_object().if_contains("oneOf") && key == "shape")
    {
        // an area's shape: circle or polygon (adventure_pack_v1.schema.json places.areas shape oneOf)
        F32 cl = l;
        fRow(l, r, top, label, cl);
        const bool circle = v.has("circle");
        const S32 ic = act([this, ptr, circle]() { if (!circle) docSet(0, ptr, boost::json::object{ { "circle", boost::json::object{ { "centre", boost::json::array{ -1, -1 } }, { "radius", 10 } } } }); });
        const S32 ip = act([this, ptr, circle]() { if (circle) docSet(0, ptr, boost::json::object{ { "polygon", boost::json::array{ boost::json::array{ -1, -1 }, boost::json::array{ -1, -1 }, boost::json::array{ -1, -1 } } } }); });
        btn(cl, top - ROW_H, cl + 90.f, top, "Circle", SH_E, ic, "A circle (centre and radius)", p, true, circle ? BTN_ACTIVE : 0);
        btn(cl + 94.f, top - ROW_H, cl + 184.f, top, "Polygon", SH_E, ip, "A polygon (3 to 64 corners) - replaces the shape", p, true, !circle ? BTN_ACTIVE : 0);
        F32 y = top - ROW_H - 6.f;
        const boost::json::value& one = s.as_object().at("oneOf");
        if (one.is_array() && one.as_array().size() >= 2)
        {
            if (circle) y = fObject(l + 10.f, r, y, ptrJoin(ptr, "circle"), prop(one.as_array()[0], "circle"), v["circle"], 0, p);
            else y = fArray(l + 10.f, r, y, ptrJoin(ptr, "polygon"), "polygon", prop(one.as_array()[1], "polygon"), v["polygon"], 0, p, "Corners");
        }
        return y;
    }
    const std::vector<std::string> en = sEnum(s);
    if (!en.empty())
    {
        const LLSD d = sDefault(s);
        return fEnum(l, r, top, label, ptr, en, v, d.isString() ? d.asString() : std::string(), doc, optional, p);
    }
    // [WOLF ADVENTURE AUDIT 2026-10-17] a choice of numbers (rules dice.sides items): the numbers as buttons, written as integers
    const std::vector<S64> enn = sEnumNums(s);
    if (!enn.empty())
    {
        F32 cl = l;
        if (label != " ") fRow(l, r, top, label, cl);
        F32 x = cl, y = top;
        for (const S64 n : enn)
        {
            const std::string w = llformat("%lld", (long long)n);
            const F32 bw = btnW(w, 40.f);
            if (x > cl && x + bw > r) { x = cl; y -= ROW_H + 4.f; }
            const bool on = (v.isInteger() || v.isReal()) && (S64)v.asInteger() == n;
            const S32 idx = act([this, doc, ptr, n]() { docSet(doc, ptr, boost::json::value((int64_t)n)); });
            btn(x, y - ROW_H, x + bw, y, w, SH_E, idx, (label == " " ? std::string("Choose ") : label + ": ") + w + (on ? " (chosen)" : ""), p, true, on ? BTN_ACTIVE : 0);
            x += bw + 4.f;
        }
        y -= ROW_H + 2.f;
        const std::string err = problemAt(ptr);
        if (!err.empty()) y = para(cl, r, y, err, DANGER_INK, true);
        return y - 6.f;
    }
    const std::string t = sType(s);
    if (name == "id" && key != "id")
    {
        const std::string kind = refKind(key, ptrLast(ptrParent(ptrParent(ptr))));
        if (!kind.empty()) return fRef(l, r, top, label, ptr, kind, v, doc, optional, p);
    }
    if (key == "region" && doc == 0) return fRef(l, r, top, label, ptr, "region", v, doc, optional, p);
    if (t == "boolean")
    {
        const LLSD d = sDefault(s);
        return fBool(l, r, top, label, ptr, v, d.isBoolean() && d.asBoolean(), doc, p, tip);
    }
    if (t == "integer" || t == "number")
    {
        F64 mn = -1e18, mx = 1e18, ex = 0.0;
        sNum(s, "minimum", mn);
        sNum(s, "maximum", mx);
        if (sNum(s, "exclusiveMinimum", ex)) mn = ex + (t == "integer" ? 1.0 : 0.001);
        return fText(l, r, top, label, ptr, v, doc, 0, t == "integer" ? 'i' : 'n', optional, tip, p, mn, mx);
    }
    if (t == "mixed")
    {
        // a flag value: Yes/No, a number or text (adventure_pack_v1.schema.json set.value, flag.default, eq/ne)
        F32 cl = l;
        fRow(l, r, top, label, cl);
        const char kindNow = v.isBoolean() ? 'b' : (v.isInteger() || v.isReal()) ? 'i' : 's';
        const S32 ib = act([this, doc, ptr]() { docSet(doc, ptr, boost::json::value(true)); });
        const S32 ii = act([this, doc, ptr]() { docSet(doc, ptr, boost::json::value((int64_t)0)); });
        const S32 is = act([this, doc, ptr]() { docSet(doc, ptr, boost::json::value(std::string())); });
        btn(cl, top - ROW_H, cl + 70.f, top, "Yes/No", SH_E, ib, "A yes/no value", p, true, kindNow == 'b' && v.isDefined() ? BTN_ACTIVE : 0);
        btn(cl + 74.f, top - ROW_H, cl + 144.f, top, "Number", SH_E, ii, "A whole number", p, true, kindNow == 'i' ? BTN_ACTIVE : 0);
        btn(cl + 148.f, top - ROW_H, cl + 218.f, top, "Text", SH_E, is, "Text", p, true, kindNow == 's' && v.isString() ? BTN_ACTIVE : 0);
        F32 y = top - ROW_H - 4.f;
        if (v.isBoolean())
        {
            const bool on = v.asBoolean();
            const S32 it = act([this, doc, ptr, on]() { docSet(doc, ptr, boost::json::value(!on)); });
            btn(cl, y - ROW_H, cl + 90.f, y, on ? "Yes" : "No", SH_E, it, label + (on ? " - now Yes" : " - now No"), p, true, on ? BTN_ACTIVE : 0);
            y -= ROW_H + 6.f;
        }
        else if (v.isDefined()) y = fText(cl, r, y, std::string(), ptr, v, doc, 240, v.isString() ? 's' : 'i', false, label, p, -1000000.0, 1000000.0);
        return y;
    }
    if (t == "array") return fArray(l, r, top, ptr, key, s, v, doc, p, label);
    if (isObjectSchema(s, t))
    {
        const std::vector<std::string> keys = propKeys(s);
        if (keys.empty())
        {
            if (key == "environment" || key == "overrides") return fText(l, r, top, label, ptr, v, doc, 0, 'j', optional, tip, p);
            const boost::json::value* ap = s.is_object() ? s.as_object().if_contains("additionalProperties") : nullptr;
            if (ap && ap->is_object() && resolve(root, *ap).is_object() && sType(resolve(root, *ap)) != "integer")
                return fMap(l, r, top, ptr, key, *ap, v, doc, p, label);
            // [WOLF ADVENTURE AUDIT 2026-10-17] a map of name -> integer whose names are NOT stats (rules creation.costs, keyed
            // by score): typed names, as the rules collections (fMap's "type its id, Enter")
            if (key != "stats" && ap && ap->is_object())
                return fMap(l, r, top, ptr, key, *ap, v, doc, p, label);
            // a map of name -> integer (a reward parcel's stats): name and value rows, Add a row
            F32 y = head(l, r, top, label, p);
            for (const auto& kv : llsd::inMap(v))
            {
                const std::string k = kv.first;
                F32 cl = l + 10.f;
                textL(small, k, cl, y - ROW_H * 0.5f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, 140.f);
                const F32 y2 = fText(cl + 150.f, r - 80.f, y, std::string(), ptrJoin(ptr, k), kv.second, doc, 0, 'i', false, label + " " + k, p, -1000000.0, 1000000.0);
                const std::string pk = ptrJoin(ptr, k);
                const S32 idx = act([this, doc, pk]() { docErase(doc, pk); });
                btn(r - 74.f, y - ROW_H, r, y, "Remove", SH_E, idx, "Remove " + k, p, true, BTN_DANGER);
                y = llmin(y2, y - ROW_H - 6.f);
            }
            const S32 ia = act([this, doc, ptr, label]()
            {
                // the stat names the rules know, as the choices (wolfGame stat names, a-z0-9_)
                std::vector<std::pair<std::string, std::string>> opts;
                for (const auto& kv : llsd::inMap(std::as_const(mRulesMerged)["stats"])) opts.push_back({ kv.first, cap(kv.second["label"].asString(), 40) + " (" + kv.first + ")" });
                openPicker("Add to " + label, opts, [this, doc, ptr](const std::string& k) { if (!k.empty()) docSet(doc, ptrJoin(ptr, k), boost::json::value((int64_t)0)); },
                           "The rules preset has no stats.");
            });
            btn(l + 10.f, y - ROW_H, l + 170.f, y, "Add a stat", SH_E, ia, "Add a stat to " + label, p);
            return y - ROW_H - 8.f;
        }
        if (!v.isDefined())
        {
            F32 cl = l;
            fRow(l, r, top, label, cl);
            const boost::json::value* rp = &root;
            const boost::json::value* sp = &schema0;
            const S32 idx = act([this, doc, ptr, rp, sp, key]()
            {
                const std::string region = std::as_const(mDraftPack)["adventure"]["regions"][0]["region"].asString();
                docSet(doc, ptr, makeDefault(*rp, *sp, key, mDraftJson, region));
                mOpen.insert(ptr);
            });
            btn(cl, top - ROW_H, cl + 160.f, top, "Add " + label, SH_E, idx, "Add " + label + (tip.empty() ? std::string() : ": " + tip), p);
            return top - ROW_H - 8.f;
        }
        F32 y = head(l, r, top, label, p);
        if (!tip.empty()) y = para(l, r, y, tip, p.mDim, true) - 2.f;
        y = fObject(l + 10.f, r, y, ptr, s, v, doc, p);
        if (optional)
        {
            const S32 idx = act([this, doc, ptr, label]() { askDelete(doc, ptr, label); });
            btn(l + 10.f, y - ROW_H, l + 170.f, y, "Remove " + label, SH_E, idx, "Remove " + label + " (asks first)", p, true, BTN_DANGER);
            y -= ROW_H + 6.f;
        }
        return y;
    }
    // text
    F64 maxlen = 240.0;
    sNum(s, "maxLength", maxlen);
    if (name == "uuid" || name == "uuidOrEmpty") maxlen = 36.0;
    return fText(l, r, top, label, ptr, v, doc, (size_t)maxlen, 's', optional || name == "uuidOrEmpty", tip, p);
}

F32 WolfAdventureStudioView::fObject(F32 l, F32 r, F32 top, const std::string& ptr, const boost::json::value& schema0, const LLSD& v, S32 doc,
                                     const Palette& p, const std::vector<std::string>& only, const std::vector<std::string>& skip)
{
    const boost::json::value& root = doc == 1 ? rulesSchema() : packSchema();
    const boost::json::value& s = resolve(root, schema0);
    F32 y = top;
    const std::vector<std::string> keys = only.empty() ? propKeys(s) : only;
    for (const std::string& k : keys)
    {
        if (std::find(skip.begin(), skip.end(), k) != skip.end()) continue;
        const boost::json::value& ps = prop(s, k);
        if (ps.is_null()) continue;
        y = fValue(l, r, y, ptrJoin(ptr, k), k, ps, v[k], !isRequired(s, k), doc, p);
    }
    return y;
}

F32 WolfAdventureStudioView::fArray(F32 l, F32 r, F32 top, const std::string& ptr, const std::string& key, const boost::json::value& schema0,
                                    const LLSD& v, S32 doc, const Palette& p, const std::string& label)
{
    const boost::json::value& root = doc == 1 ? rulesSchema() : packSchema();
    const boost::json::value& s = resolve(root, schema0);
    const boost::json::value* items = s.is_object() ? s.as_object().if_contains("items") : nullptr;
    const boost::json::value& isch0 = items ? *items : nullV();
    std::string iname;
    const boost::json::value& isch = resolve(root, isch0, &iname);
    const std::string itype = sType(isch);
    const S32 n = v.isArray() ? (S32)v.size() : 0;
    F64 maxItems = 1e9;
    sNum(s, "maxItems", maxItems);
    F32 y = head(l, r, top, label + (n ? llformat(" (%d)", n) : std::string()), p);
    const std::string tip = sDescription(s);
    if (!tip.empty()) y = para(l, r, y, tip, p.mDim, true) - 2.f;
    const std::string err = problemAt(ptr);
    if (!err.empty()) y = para(l, r, y, err, DANGER_INK, true) - 2.f;
    const std::string lkind = listKind(key);
    // [WOLF ADVENTURE AUDIT 2026-10-17] an item that is a choice (an enum, of words or numbers) is one row too
    const bool scalar = itype == "string" || itype == "integer" || itype == "number" || iname == "vec2" || iname == "vec3" || iname == "id"
                     || !sEnum(isch).empty() || !sEnumNums(isch).empty();
    for (S32 i = 0; i < n; ++i)
    {
        const std::string ip = ptrIndex(ptr, i);
        if (scalar)
        {
            // one row: the value, and Up / Down / Remove on its right
            const F32 bx = r - 200.f;
            F32 y2 = y;
            if (!lkind.empty() || (iname == "id" && !refKind(key, std::string()).empty()))
            {
                const std::string kind = lkind.empty() ? refKind(key, std::string()) : lkind;
                y2 = fRef(l + 10.f, bx - 6.f, y, std::string(), ip, kind, v[i], doc, false, p);
            }
            else y2 = fValue(l + 10.f, bx - 6.f, y, ip, key, isch0, v[i], false, doc, p, " ");
            const S32 iu = act([this, doc, ptr, i]() { docMove(doc, ptr, i, -1); });
            const S32 id = act([this, doc, ptr, i]() { docMove(doc, ptr, i, 1); });
            const S32 ix = act([this, doc, ip]() { docErase(doc, ip); });
            btn(bx, y - ROW_H, bx + 50.f, y, "Up", SH_E, iu, "Move up", p, i > 0);
            btn(bx + 54.f, y - ROW_H, bx + 116.f, y, "Down", SH_E, id, "Move down", p, i + 1 < n);
            btn(bx + 120.f, y - ROW_H, r, y, "Remove", SH_E, ix, "Remove this one", p, true, BTN_DANGER);
            y = llmin(y2, y - ROW_H - 4.f);
            continue;
        }
        const LLSD& it = v[i];
        std::string title = it["label"].asString();
        if (title.empty()) title = it["title"].asString();
        if (title.empty()) title = it["name"].asString();
        if (title.empty()) title = it["prompt"].asString();
        if (title.empty()) title = it["text"].asString();
        if (title.empty()) title = it["id"].asString();
        if (title.empty()) title = it["cast"].asString();
        if (title.empty()) title = it["cue"].asString();
        title = labelFor(idBase(key)) + llformat(" %d", i + 1) + (title.empty() ? std::string() : ": " + cap(title, 60));
        y = cardHead(l, r, y, ip, title, i, n, ptr, true, p, true, doc);
        if (mOpen.count(ip)) y = fObject(l + 14.f, r, y, ip, isch0, it, doc, p) - 4.f;
    }
    if (doc == 0 || doc == 1)
    {
        const std::string next = ptrIndex(ptr, n);
        const boost::json::value* rp = &root;
        const boost::json::value* ip = &isch0;
        const S32 idx = act([this, doc, ptr, rp, ip, next, lkind, key]()
        {
            if (!lkind.empty())
            {
                // a list of references: pick what to add (§2.3 "the story cards this character may know (checkboxes)")
                std::vector<std::pair<std::string, std::string>> opts = refOptions(lkind, next);
                const LLSD& cur = sdAt(docRead(doc), ptr);
                opts.erase(std::remove_if(opts.begin(), opts.end(), [&cur](const std::pair<std::string, std::string>& o)
                {
                    for (const LLSD& x : llsd::inArray(cur)) if (x.asString() == o.first) return true;
                    return false;
                }), opts.end());
                openPicker("Add to " + labelFor(key), opts, [this, doc, ptr](const std::string& val)
                {
                    if (!val.empty()) docAppend(doc, ptr, boost::json::value(val));
                }, "Nothing more to add.");
                return;
            }
            const std::string region = std::as_const(mDraftPack)["adventure"]["regions"][0]["region"].asString();
            const LLSD& cur = sdAt(docRead(doc), ptr);
            docAppend(doc, ptr, makeDefault(*rp, *ip, key, mDraftJson, region, cur.isArray() ? (S32)cur.size() : 0));
            mOpen.insert(next);
        });
        const std::string add = "Add " + (lkind.empty() ? addWordsFor(key) : std::string("from the list..."));
        btn(l + 10.f, y - ROW_H, l + 10.f + btnW(add, 150.f), y, add, SH_E, idx,
            add + (n >= (S32)maxItems ? " (the most this list can hold)" : std::string()), p, n < (S32)maxItems);
        y -= ROW_H + 8.f;
    }
    return y;
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// the builders (STUDIO §2.8): the condition builder, the effect list, and the trigger list (§2.5)
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

namespace
{
    // Source: ADVENTURE_PACK_V1.md §11.3 (subjects, comparators) and STUDIO §2.8's leaf rows.
    struct CondKind { const char* mKey; const char* mWords; const char* mTemplate; };
    const CondKind COND_KINDS[] = {
        { "all", "All of these", "{\"all\":[{\"flag\":\"\"}]}" },
        { "any", "Any of these", "{\"any\":[{\"flag\":\"\"}]}" },
        { "not", "Not this", "{\"not\":{\"flag\":\"\"}}" },
        { "flag", "Flag ... is ...", "{\"flag\":\"\"}" },
        { "rep", "Reputation with ... is at least ...", "{\"rep\":\"\",\"gte\":0}" },
        { "castRep", "A character's opinion is at least ...", "{\"castRep\":\"\",\"gte\":0}" },
        { "honour", "Honour is in band ... / at least ...", "{\"honour\":true,\"gte\":0}" },
        { "visited", "Has visited ... at least ... times", "{\"visited\":\"\"}" },
        { "item", "Holds ... of ...", "{\"item\":\"\",\"gte\":1}" },
        { "stat", "A stat is at least ...", "{\"stat\":\"\",\"gte\":0}" },
        { "partySize", "The party has at least ... present", "{\"partySize\":true,\"gte\":1}" },
        { "hour", "Between ... and ... o'clock", "{\"hour\":[8,18]}" },
        { "gmPresent", "A host is there", "{\"gmPresent\":true}" },
        { "aiMode", "AI is on / off", "{\"aiMode\":\"ai\"}" },
        { "chance", "With chance ...%", "{\"chance\":0.5}" } };
    std::string condKind(const LLSD& v)
    {
        for (const CondKind& k : COND_KINDS) if (v.has(k.mKey)) return k.mKey;
        return std::string();
    }
    const char* COMPARATORS[] = { "eq", "ne", "gte", "lte", "gt", "lt" };
    const char* cmpWords(const std::string& c)
    {
        if (c == "eq") return "is";
        if (c == "ne") return "is not";
        if (c == "gte") return "is at least";
        if (c == "lte") return "is at most";
        if (c == "gt") return "is more than";
        if (c == "lt") return "is less than";
        if (c == "band") return "is in band";
        return "is set";
    }
}

F32 WolfAdventureStudioView::fCondition(F32 l, F32 r, F32 top, const std::string& ptr, const LLSD& v, S32 doc, const Palette& p, S32 depth)
{
    const std::string kind = condKind(v);
    F32 y = top;
    // the kind of rule
    {
        std::string words = "Choose...";
        for (const CondKind& k : COND_KINDS) if (kind == k.mKey) words = k.mWords;
        F32 cl = l;
        fRow(l, r, y, depth > 1 ? "Rule" : "Rule kind", cl);
        const S32 idx = act([this, ptr]()
        {
            std::vector<std::pair<std::string, std::string>> opts;
            for (const CondKind& k : COND_KINDS) opts.push_back({ k.mTemplate, k.mWords });
            openPicker("Kind of rule", opts, [this, ptr](const std::string& tpl)
            {
                boost::system::error_code ec;
                boost::json::value v2 = boost::json::parse(tpl, ec);
                if (ec) return;
                // a group (All / Any / Not) with rules in it is not thrown away without asking
                const LLSD& cur = sdAt(mDraftPack, ptr);
                const std::string k = condKind(cur);
                const S32 inside = (k == "all" || k == "any") && cur[k].isArray() ? (S32)cur[k].size() : (k == "not" ? 1 : 0);
                if (inside > 0)
                {
                    mCondPtr = ptr;
                    mCondNew = tpl;
                    mConfirmKeyA = llformat("%d", inside);
                    ask(SC_COND_REPLACE, 0);
                    return;
                }
                docSet(0, ptr, v2);
            }, std::string());
        });
        btn(cl, y - ROW_H, r, y, words, SH_E, idx, "What this rule tests (STUDIO §2.8 condition builder)", p);
        y -= ROW_H + 4.f;
        const std::string err = problemAt(ptr);
        if (!err.empty()) y = para(cl, r, y, err, DANGER_INK, true) - 2.f;
    }
    if (kind == "all" || kind == "any")
    {
        const std::string ap = ptrJoin(ptr, kind);
        const LLSD& list = v[kind];
        const S32 n = list.isArray() ? (S32)list.size() : 0;
        for (S32 i = 0; i < n; ++i)
        {
            const std::string cp = ptrIndex(ap, i);
            rect(l + 4.f, y - 2.f, l + 6.f, y - 24.f, fade(p.mAccent, 0.6f));
            y = fCondition(l + 14.f, r, y, cp, list[i], doc, p, depth + 1);
            const S32 ix = act([this, cp]() { docErase(0, cp); });
            btn(l + 14.f, y - 22.f, l + 134.f, y, "Remove this rule", SH_E, ix, "Remove this rule from the group", p, n > 1, BTN_DANGER);
            y -= 28.f;
        }
        const S32 ia = act([this, ap]() { docAppend(0, ap, boost::json::object{ { "flag", "" } }); });
        btn(l + 14.f, y - ROW_H, l + 134.f, y, "Add a rule", SH_E, ia, std::string("Add a rule to ") + (kind == "all" ? "All of these" : "Any of these"), p,
            n < 16);   // adventure_pack_v1.schema.json condition all/any maxItems 16
        return y - ROW_H - 6.f;
    }
    if (kind == "not")
    {
        rect(l + 4.f, y - 2.f, l + 6.f, y - 24.f, fade(p.mAccent, 0.6f));
        return fCondition(l + 14.f, r, y, ptrJoin(ptr, "not"), v["not"], doc, p, depth + 1);
    }
    if (kind.empty()) return y;
    const boost::json::value& cs = def(packSchema(), "condition");
    // the subject
    if (kind == "flag" || kind == "rep" || kind == "castRep" || kind == "visited")
    {
        const std::string rk = kind == "flag" ? "flag" : kind == "rep" ? "faction" : kind == "castRep" ? "cast" : "visited";
        y = fRef(l, r, y, kind == "flag" ? "Flag" : kind == "rep" ? "Faction" : kind == "castRep" ? "Character" : "Beat or scene", ptrJoin(ptr, kind), rk, v[kind], 0, false, p);
    }
    else if (kind == "item" || kind == "stat")
    {
        y = fText(l, r, y, kind == "item" ? "Item (wolfGame name)" : "Stat (wolfGame name)", ptrJoin(ptr, kind), v[kind], 0, 24, 's', false,
                  "a-z, 0-9 and _, up to 24 (adventure_pack_v1.schema.json)", p);
    }
    else if (kind == "hour")
    {
        y = fValue(l, r, y, ptrJoin(ptr, "hour"), "hour", prop(cs, "hour"), v["hour"], false, 0, p, "Region hours (from, to)");
        return y;
    }
    else if (kind == "gmPresent") return fBool(l, r, y, "A host is there", ptrJoin(ptr, "gmPresent"), v["gmPresent"], true, 0, p);
    else if (kind == "aiMode") return fEnum(l, r, y, "AI mode in force", ptrJoin(ptr, "aiMode"), { "ai", "none" }, v["aiMode"], "ai", 0, false, p);
    else if (kind == "chance")
    {
        y = fText(l, r, y, "Chance (0 to 1)", ptrJoin(ptr, "chance"), v["chance"], 0, 0, 'n', false, "0.25 = 25%", p, 0.0, 1.0);
        if (v["chance"].isReal() || v["chance"].isInteger())
        {
            textL(LLFontGL::getFontSansSerifSmall(), llformat("= %.0f%%", v["chance"].asReal() * 100.0), l + LABEL_W, y, p.mDim);
            y -= 16.f;
        }
        return y;
    }
    // the comparator (§11.3: one, for the value subjects; band for rep / castRep / honour)
    std::string cmp;
    for (const char* c : COMPARATORS) if (v.has(c)) cmp = c;
    if (v.has("band")) cmp = "band";
    const bool may_bare = kind == "flag" || kind == "visited";
    const bool may_band = kind == "rep" || kind == "castRep" || kind == "honour";
    {
        F32 cl = l;
        fRow(l, r, y, "Test", cl);
        const LLSD keep = v;
        const S32 idx = act([this, ptr, may_bare, may_band, kind, keep]()
        {
            std::vector<std::pair<std::string, std::string>> opts;
            if (may_bare) opts.push_back({ "", kind == "flag" ? "is set (true, non-zero or non-empty)" : "at least once" });
            for (const char* c : COMPARATORS) opts.push_back({ c, cmpWords(c) });
            if (may_band) opts.push_back({ "band", "is in band" });
            openPicker("Test", opts, [this, ptr, keep, kind](const std::string& c)
            {
                boost::json::object o;
                o[kind] = toJson(keep[kind]);
                if (keep.has("honour")) o["honour"] = true;
                if (keep.has("partySize")) o["partySize"] = true;
                if (c == "band") o["band"] = "";
                else if (!c.empty()) o[c] = (c == "eq" || c == "ne") ? boost::json::value(true) : boost::json::value((int64_t)0);
                docSet(0, ptr, o);
            }, std::string());
        });
        btn(cl, y - ROW_H, r, y, cmpWords(cmp), SH_E, idx, "How the value is compared", p);
        y -= ROW_H + 4.f;
    }
    if (cmp == "band") return fRef(l, r, y, "Band", ptrJoin(ptr, "band"), "band", v["band"], 0, false, p);
    if (cmp == "eq" || cmp == "ne")
    {
        // typed by the flag's own type when the subject is a flag (bool / int / text)
        std::string ftype;
        if (kind == "flag")
            for (const LLSD& f : llsd::inArray(std::as_const(mDraftPack)["state"]["flags"])) if (f["id"].asString() == v["flag"].asString()) ftype = f["type"].asString();
        const std::string vp = ptrJoin(ptr, cmp);
        if (ftype == "bool") return fBool(l, r, y, "Value", vp, v[cmp], true, 0, p);
        if (ftype == "int") return fText(l, r, y, "Value", vp, v[cmp], 0, 0, 'i', false, "a whole number", p);
        if (ftype == "text") return fText(l, r, y, "Value", vp, v[cmp], 0, 240, 's', false, "the text", p);
        return fValue(l, r, y, vp, cmp, prop(cs, cmp), v[cmp], false, 0, p, "Value");
    }
    if (!cmp.empty()) return fText(l, r, y, "Value", ptrJoin(ptr, cmp), v[cmp], 0, 0, 'i', false, "a whole number", p);
    return y;
}

F32 WolfAdventureStudioView::fEffects(F32 l, F32 r, F32 top, const std::string& ptr, const LLSD& v, const Palette& p, const std::string& label)
{
    const boost::json::value& es = def(packSchema(), "effect");
    const S32 n = v.isArray() ? (S32)v.size() : 0;
    F32 y = head(l, r, top, label + (n ? llformat(" (%d steps)", n) : std::string()), p);
    const std::string err = problemAt(ptr);
    if (!err.empty()) y = para(l, r, y, err, DANGER_INK, true) - 2.f;
    for (S32 i = 0; i < n; ++i)
    {
        const std::string ip = ptrIndex(ptr, i);
        const LLSD& e = v[i];
        std::string op;
        for (const auto& kv : llsd::inMap(e)) if (kv.first != "if") op = kv.first;
        std::string what = opSentence(op);
        // a short summary in the head, so a closed list still reads as a story
        const LLSD& ov = e[op];
        if (ov.isString() && !ov.asString().empty()) what += ": " + cap(ov.asString(), 40);
        else if (ov.isMap() && ov.has("text")) what += ": " + cap(ov["text"].asString(), 40);
        else if (ov.isMap() && ov.has("flag")) what += ": " + cap(ov["flag"].asString(), 30);
        else if (ov.isReal() || ov.isInteger()) what += ": " + valueText(ov);
        if (e.has("if")) what += " (only if...)";
        y = cardHead(l, r, y, ip, llformat("Step %d. ", i + 1) + what, i, n, ptr, true, p);
        if (!mOpen.count(ip)) continue;
        const F32 il = l + 14.f;
        if (!op.empty()) y = fValue(il, r, y, ptrJoin(ip, op), op, prop(es, op), ov, false, 0, p, opSentence(op));
        y = fValue(il, r, y, ptrJoin(ip, "if"), "if", prop(es, "if"), e["if"], true, 0, p, "Only if");
        y -= 4.f;
    }
    const boost::json::value* esp = &es;
    const S32 idx = act([this, ptr, n, esp]()
    {
        std::vector<std::pair<std::string, std::string>> opts;
        for (const Op& o : OPS) if (!prop(*esp, o.mOp).is_null()) opts.push_back({ o.mOp, o.mSentence });   // only steps this schema has
        openPicker("Add a step", opts, [this, ptr, n, esp](const std::string& op)
        {
            if (op.empty()) return;
            const std::string region = std::as_const(mDraftPack)["adventure"]["regions"][0]["region"].asString();
            boost::json::object e;
            e[op] = makeDefault(packSchema(), prop(*esp, op), op, mDraftJson, region);
            docAppend(0, ptr, e);
            mOpen.insert(ptrIndex(ptr, n));
        }, std::string());
    });
    btn(l + 10.f, y - ROW_H, l + 170.f, y, "Add step...", SH_E, idx, "Add a step to " + label + " (every op of ADVENTURE_PACK_V1.md §11.2)", p,
        n < 32);   // adventure_pack_v1.schema.json effects maxItems 32
    return y - ROW_H - 8.f;
}

F32 WolfAdventureStudioView::fTriggers(F32 l, F32 r, F32 top, const std::string& ptr, const LLSD& v, const Palette& p)
{
    const boost::json::value& ts = def(packSchema(), "trigger");
    const S32 n = v.isArray() ? (S32)v.size() : 0;
    F32 y = head(l, r, top, "Triggers" + (n ? llformat(" (%d)", n) : std::string()), p);
    const std::string err = problemAt(ptr);
    if (!err.empty()) y = para(l, r, y, err, DANGER_INK, true) - 2.f;
    for (S32 i = 0; i < n; ++i)
    {
        const std::string ip = ptrIndex(ptr, i);
        const LLSD& t = v[i];
        const TriggerType* tt = triggerType(t["on"].asString());
        std::string title = tt ? tt->mSentence : std::string("A trigger");
        if (t.has("next")) title += " -> " + cap(t["next"].asString(), 30);
        y = cardHead(l, r, y, ip, title + " (" + cap(t["id"].asString(), 30) + ")", i, n, ptr, true, p);
        if (!mOpen.count(ip)) continue;
        const F32 il = l + 14.f;
        // When: the type, as a plain sentence (changing it keeps only the fields the new type uses)
        {
            F32 cl = il;
            fRow(il, r, y, "When", cl);
            const LLSD keep = t;
            const S32 idx = act([this, ip, keep]()
            {
                std::vector<std::pair<std::string, std::string>> opts;
                for (const TriggerType& x : TRIGGERS) if (triggerKnown(x.mOn)) opts.push_back({ x.mOn, x.mSentence });
                openPicker("When", opts, [this, ip, keep](const std::string& on)
                {
                    if (on.empty()) return;
                    const TriggerType* nt = triggerType(on);
                    boost::json::object o;
                    o["id"] = toJson(keep["id"]);
                    o["on"] = on;
                    for (const std::string& f : splitComma(nt ? nt->mFields : ""))
                        if (keep.has(f)) o[f] = toJson(keep[f]);
                    for (const char* k : { "if", "who", "once", "do", "next" }) if (keep.has(k)) o[k] = toJson(keep[k]);
                    docSet(0, ip, o);
                }, std::string());
            });
            btn(cl, y - ROW_H, r, y, tt ? tt->mSentence : "Choose...", SH_E, idx, "What makes this trigger fire (ADVENTURE_PACK_V1.md §11.1)", p);
            y -= ROW_H + 6.f;
        }
        y = fValue(il, r, y, ptrJoin(ip, "id"), "id", prop(ts, "id"), t["id"], false, 0, p);
        if (tt)
            for (const std::string& f : splitComma(tt->mFields))
            {
                const bool needed = f == "area" || f == "object" || f == "cast" || f == "intent" || f == "decision" || f == "item" || f == "sec"
                                 || f == "label" || f == "battle" || f == "check" || f == "result" || f == "hours" || f == "quiz" || f == "scene";
                y = fValue(il, r, y, ptrJoin(ip, f), f, prop(ts, f), t[f], !needed, 0, p);
            }
        // `flag` triggers fire when their `if` becomes true: the condition is the trigger itself (§11.1 table)
        const bool flagType = t["on"].asString() == "flag";
        y = fValue(il, r, y, ptrJoin(ip, "if"), "if", prop(ts, "if"), t["if"], !flagType, 0, p, flagType ? "When this becomes true" : "Only if");
        y = fValue(il, r, y, ptrJoin(ip, "who"), "who", prop(ts, "who"), t["who"], true, 0, p);
        y = fValue(il, r, y, ptrJoin(ip, "once"), "once", prop(ts, "once"), t["once"], true, 0, p);
        y = fEffects(il, r, y, ptrJoin(ip, "do"), t["do"], p, "Do");
        y = fValue(il, r, y, ptrJoin(ip, "next"), "next", prop(ts, "next"), t["next"], true, 0, p, "Then go to");
        y -= 4.f;
    }
    const S32 idx = act([this, ptr, n]()
    {
        std::vector<std::pair<std::string, std::string>> opts;
        for (const TriggerType& x : TRIGGERS) if (triggerKnown(x.mOn)) opts.push_back({ x.mOn, x.mSentence });
        openPicker("Add trigger", opts, [this, ptr, n](const std::string& on)
        {
            if (on.empty()) return;
            boost::json::object o;
            o["id"] = uniqueId("trigger", mDraftJson);
            o["on"] = on;
            if (on == "flag") o["if"] = boost::json::object{ { "flag", "" } };
            if (on == "timer") o["sec"] = 60;
            if (on == "say") o["intent"] = boost::json::array();
            docAppend(0, ptr, o);
            mOpen.insert(ptrIndex(ptr, n));
        }, std::string());
    });
    btn(l + 10.f, y - ROW_H, l + 170.f, y, "Add trigger...", SH_E, idx, "Add a trigger to this beat", p, n < 32);   // beat triggers maxItems 32
    return y - ROW_H - 8.f;
}

/** An object keyed by id (a rules collection): each entry a card; changed entries say so; Reset, Remove, Add. */
F32 WolfAdventureStudioView::fMap(F32 l, F32 r, F32 top, const std::string& ptr, const std::string& key, const boost::json::value& itemSchema,
                                  const LLSD& v, S32 doc, const Palette& p, const std::string& label)
{
    const boost::json::value& root = doc == 1 ? rulesSchema() : packSchema();
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    F32 y = head(l, r, top, label, p);
    for (const auto& kv : llsd::inMap(v))
    {
        const std::string ep = ptrJoin(ptr, kv.first);
        const bool open = mOpen.count(ep) > 0;
        const F32 h = 28.f;
        rect(l, y - h, r, y, fade(p.mCard, 0.9f));
        F32 x = r;
        auto sb = [&](const std::string& lab, const std::string& tip, const std::function<void()>& fn, bool enabled, U32 extra)
        {
            const F32 bw = btnW(lab);
            x -= bw + 3.f;
            const S32 idx = act(fn);
            btn(x, y - h + 2.f, x + bw, y - 2.f, lab, SH_E, idx, tip, p, enabled, extra);
        };
        const bool changed = doc == 1 && rulesOverridden(ep);
        const bool inPreset = doc == 1 && sdAt(mPreset, ep).isDefined();
        sb("Remove", "Remove " + kv.first + " (asks first)", [this, doc, ep, kv]() { askDelete(doc, ep, kv.first); }, true, BTN_DANGER);
        if (doc == 1)
            sb("Reset to preset", "Put " + kv.first + " back as the preset has it", [this, ep]() { const boost::json::value pv = toJson(sdAt(mPreset, ep)); rulesSet(ep, &pv); },
               changed && inPreset, 0);
        const S32 ti = act([this, ep, open]() { if (open) mOpen.erase(ep); else mOpen.insert(ep); });
        chevron(l + 12.f, y - h * 0.5f, open, p.mText);
        hit(l, y - h, x - 4.f, y, SH_E, ti, (open ? "Close " : "Open ") + kv.first);
        if (mKeyboardNav && hasFocus() && mHits.isFocused(SH_E, ti)) focusRing(l, y - h, x - 4.f, y, p);
        std::string title = kv.first;
        if (kv.second.isMap() && kv.second.has("label")) title = cap(kv.second["label"].asString(), 40) + " (" + kv.first + ")";
        F32 tx = textL(LLFontGL::getFontSansSerifSmallBold(), title, l + 24.f, y - h * 0.5f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, x - l - 120.f);
        if (changed)
        {
            // §2.6 "An edited value shows a dot and 'changed' next to it"
            disc(tx + 10.f, y - h * 0.5f, 3.5f, WARN_INK);
            textL(small, inPreset ? "changed" : "added", tx + 18.f, y - h * 0.5f, WARN_INK, LLFontGL::LEFT, LLFontGL::VCENTER);
        }
        y -= h + 4.f;
        if (open) y = fValue(l + 14.f, r, y, ep, kv.first, itemSchema, kv.second, false, doc, p, " ") - 4.f;
    }
    // Add: type the new id (a-z, 0-9 and _)
    const std::string region = std::as_const(mDraftPack)["adventure"]["regions"][0]["region"].asString();
    // a rules entry's id is its key: no pack-wide id scan is needed for its default
    const boost::json::value dflt = makeDefault(root, itemSchema, key, std::string(), region);
    F32 cl = l;
    fRow(l, r, y, "Add (type its id, Enter)", cl);
    const S32 idx = textAct(ptr, doc, 24, 'k', true);
    mActs[idx].mExtra = boost::json::serialize(dflt);
    const bool editing = mField == F_EDIT && mEditPtr == ptr && mEditType == 'k';
    const bool invalid = mEditErrorPtr == ptr && !mEditError.empty();
    box(cl, y - ROW_H, r, y, editing && hasFocus(), invalid, p);
    const LLFontGL* f = LLFontGL::getFontSansSerif();
    const F32 cy = y - ROW_H * 0.5f;
    F32 cx = cl + 8.f;
    if (editing && !mEditBuf.empty()) cx = textL(f, mEditBuf, cl + 8.f, cy, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER);
    else textL(f, "new id, e.g. luck", cl + 8.f, cy, p.mDim, LLFontGL::LEFT, LLFontGL::VCENTER);
    if (editing && hasFocus() && fmod(now(), 1.0) < 0.6) rect(cx + 1.f, cy - 7.f, cx + 2.f, cy + 7.f, p.mText);
    hit(cl, y - ROW_H, r, y, SH_E_TEXT, idx, "Type the new " + labelFor(idBase(key)) + "'s id and press Enter");
    y -= ROW_H + 2.f;
    if (invalid) y = para(cl, r, y, mEditError, DANGER_INK, true);
    return y - 8.f;
}

/** STUDIO §2.3 "Day: the schedule as a 24-hour ribbon. Click a time range, then pick a spot or path, or Away." */
F32 WolfAdventureStudioView::scheduleRibbon(F32 l, F32 r, F32 top, const std::string& ptr, const LLSD& v, const Palette& p)
{
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    F32 y = head(l, r, top, "Day (region hours)", p);
    y = para(l, r, y, "Click the first hour, then the last, then choose where they are. First matching entry wins; any other time they are at home.", p.mDim, true) - 4.f;
    const F32 cw = (r - l) / 24.f, h = 30.f;
    static const LLColor4 SWATCH[4] = { LLColor4(0.96f, 0.65f, 0.14f, 1.f), LLColor4(0.49f, 0.83f, 0.99f, 1.f), LLColor4(0.29f, 0.87f, 0.50f, 1.f), LLColor4(0.85f, 0.55f, 0.95f, 1.f) };
    for (S32 hr = 0; hr < 24; ++hr)
    {
        const F32 x0 = l + hr * cw, x1 = x0 + cw - 1.f;
        // the entry covering this hour (first match wins: ADVENTURE_PACK_V1.md §7 schedule)
        S32 which = -1;
        for (size_t i = 0; v.isArray() && i < (size_t)v.size() && which < 0; ++i)
        {
            const F64 from = v[(LLSD::Integer)i]["from"].asReal(), to = v[(LLSD::Integer)i]["to"].asReal();
            const F64 mid = hr + 0.5;
            const bool in = from <= to ? (mid >= from && mid < to) : (mid >= from || mid < to);
            if (in) which = (S32)i;
        }
        const bool pending = mRibbonFrom >= 0 && hr == mRibbonFrom;
        rect(x0, y - h, x1, y, which >= 0 ? fade(SWATCH[which % 4], 0.75f) : fade(p.mTrack, 0.9f));
        if (pending) chamferEdge(x0, y - h, x1, y, 2.f, 2.f, p.mAccent);
        if (hr % 3 == 0) textL(small, llformat("%d", hr), x0 + 2.f, y - h - 2.f, p.mDim);
        const S32 idx = act([this, ptr, hr, v]()
        {
            // adventure_pack_v1.schema.json castSheet schedule: maxItems 12, hours 0-24
            if (v.isArray() && v.size() >= 12) { mRibbonFrom = -1; setStatus("A day has at most 12 entries: remove one below first.", true); return; }
            if (mRibbonFrom < 0) { mRibbonFrom = hr; setStatus(llformat("From %d:00 - now click the last hour.", hr), false); return; }
            // to the end of the last hour clicked (24 = midnight); a last hour before the first runs over midnight
            const S32 from = mRibbonFrom, to = hr + 1;
            mRibbonFrom = -1;
            std::vector<std::pair<std::string, std::string>> opts;
            opts.push_back({ "~away", "Away (not in the world)" });
            for (const auto& o : refOptions("spot", ptr)) opts.push_back({ "spot:" + o.first, "At " + o.second });
            for (const auto& o : refOptions("path", ptr)) opts.push_back({ "path:" + o.first, "Walking " + o.second });
            openPicker(llformat("From %d:00 to %d:00", from, to), opts, [this, ptr, from, to](const std::string& val)
            {
                if (val.empty()) return;
                boost::json::object e;
                e["from"] = from;
                e["to"] = to;
                if (val == "~away") e["absent"] = true;
                else if (val.rfind("spot:", 0) == 0) e["spot"] = val.substr(5);
                else e["path"] = val.substr(5);
                docAppend(0, ptr, e);
            }, "Add spots or paths on the Places page first.");
        });
        hit(x0, y - h, x1, y, SH_E, idx, llformat("%d:00 - %s", hr, which >= 0 ? (v[which]["absent"].asBoolean() ? "away" : (v[which].has("spot") ? ("at " + v[which]["spot"].asString()).c_str() : ("walking " + v[which]["path"].asString()).c_str())) : "at home"));
        if (mKeyboardNav && hasFocus() && mHits.isFocused(SH_E, idx)) focusRing(x0, y - h, x1, y, p);
    }
    y -= h + (F32)small->getLineHeight() + 8.f;
    // the entries themselves, editable (the keyboard route, §2.3 "The ribbon is keyboard-operable")
    const boost::json::value& cs = def(packSchema(), "castSheet");
    return fArray(l, r, y, ptr, "schedule", prop(cs, "schedule"), v, 0, p, "Schedule entries");
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// the pages
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

namespace
{
    F32 waiting(F32 l, F32 r, F32 top, const std::string& text, const Palette& p)
    {
        textL(LLFontGL::getFontSansSerif(), text, l, top - 6.f, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, r - l);
        return top - 30.f;
    }
}

F32 WolfAdventureStudioView::pageOutline(F32 l, F32 r, F32 top, const Palette& p)
{
    if (!openIsOwner()) return waiting(l, r, top, "Only the adventure's owner edits its story.", p);
    if (mDraftJson.empty()) return waiting(l, r, top, "Opening the draft...", p);
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    F32 y = head(l, r, conflictBanner(l, r, top, p), "The story", p);
    y = para(l, r, y, "Acts hold scenes, scenes hold beats (the step the players are on). Select one to work on it; Open edits it on Advanced editing > Story.", p.mDim, true) - 4.f;
    const LLSD& acts = std::as_const(mDraftPack)["acts"];
    if (mSel.empty() && acts.isArray() && acts.size() > 0) mSel = "/acts/0";
    // the tree (§2.2), each node with its problem count (an icon and a number)
    auto row = [&](const std::string& ptr, S32 depth, const std::string& text)
    {
        const F32 h = 24.f, x0 = l + depth * 18.f;
        const bool sel = mSel == ptr;
        if (sel) rect(x0, y - h, r, y, fade(p.mAccent, 0.22f));
        const S32 idx = act([this, ptr]() { mSel = ptr; });
        const S32 n = problemsUnder(ptr);
        F32 te = r - 6.f;
        if (n > 0)
        {
            const std::string pw = llformat("%d", n);
            te -= (F32)bold->getWidthF32(pw) + 18.f;
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            gGL.begin(LLRender::TRIANGLES);
            gGL.color4fv(WARN_INK.mV);
            const F32 cy = y - h * 0.5f;
            gGL.vertex2f(te + 2.f, cy - 5.f); gGL.vertex2f(te + 12.f, cy - 5.f); gGL.vertex2f(te + 7.f, cy + 5.f);
            gGL.end();
            textL(bold, pw, te + 15.f, cy, WARN_INK, LLFontGL::LEFT, LLFontGL::VCENTER);
        }
        textL(depth == 0 ? bold : small, text, x0 + 6.f, y - h * 0.5f, sel ? p.mAccent : p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, te - x0 - 10.f);
        hit(x0, y - h, r, y, SH_E, idx, "Select " + text + (n ? llformat(" (%d problems)", n) : std::string()));
        if (mKeyboardNav && hasFocus() && mHits.isFocused(SH_E, idx)) focusRing(x0, y - h, r, y, p);
        y -= h + 2.f;
    };
    for (size_t a = 0; acts.isArray() && a < (size_t)acts.size(); ++a)
    {
        const LLSD& act_ = acts[(LLSD::Integer)a];
        const std::string ap = ptrIndex("/acts", (S32)a);
        row(ap, 0, llformat("Act %d: ", (S32)a + 1) + cap(act_["title"].asString(), 60));
        const LLSD& scenes = act_["scenes"];
        for (size_t s = 0; scenes.isArray() && s < (size_t)scenes.size(); ++s)
        {
            const LLSD& sc = scenes[(LLSD::Integer)s];
            const std::string sp = ptrIndex(ap + "/scenes", (S32)s);
            row(sp, 1, "Scene: " + cap(sc["title"].asString(), 60));
            const LLSD& beats = sc["beats"];
            for (size_t b = 0; beats.isArray() && b < (size_t)beats.size(); ++b)
                row(ptrIndex(sp + "/beats", (S32)b), 2, "Beat: " + cap(beats[(LLSD::Integer)b]["objective"].asString(), 70) + " (" + beats[(LLSD::Integer)b]["id"].asString() + ")");
        }
    }
    // the tools for the selection (§2.2 "add, duplicate and delete at any level; reorder by ... Move up / Move down")
    const std::vector<std::string> t = ptrTokens(mSel);
    const S32 level = t.size() >= 6 ? 2 : t.size() >= 4 ? 1 : t.size() >= 2 ? 0 : -1;
    const std::string arr = ptrParent(mSel);
    const S32 idxSel = t.empty() ? -1 : tokIndex(t.back());
    const S32 count = level >= 0 && sdAt(mDraftPack, arr).isArray() ? (S32)sdAt(mDraftPack, arr).size() : 0;
    const boost::json::value& root = packSchema();
    F32 x = l;
    auto tool = [&](const std::string& lab, const std::string& tip, const std::function<void()>& fn, bool enabled, U32 extra = 0)
    {
        const F32 bw = btnW(lab);
        if (x + bw > r) { x = l; y -= ROW_H + 4.f; }
        const S32 idx = act(fn);
        btn(x, y - ROW_H, x + bw, y, lab, SH_E, idx, tip, p, enabled, extra);
        x += bw + 4.f;
    };
    y -= 4.f;
    // [WOLF ADVENTURE AUDIT 2026-10-17] a new act, scene or beat is put INTO the story's chain (addStoryItem), so it is never
    // born unreachable or a dead end (brain wolfadventure_lib.php:1387, :1390 - both errors)
    const std::string sel = mSel;
    tool("Add act", "Add an act at the end; its first beat follows the story's last beat", [this, sel]() { addStoryItem(0, sel); }, true);
    if (level >= 0)
        tool("Add scene", "Add a scene at the end of this act; its first beat follows the act's last beat", [this, sel]() { addStoryItem(1, sel); }, true);
    if (level >= 1)
        tool("Add beat", level == 2 ? "Add a beat right after this one, in the same chain" : "Add a beat at the end of this scene, in the chain",
             [this, sel]() { addStoryItem(2, sel); }, true);
    if (level >= 0)
    {
        tool("Open", "Edit it on Advanced editing > Story", [this]() { mPage = SP_STORY; mScroll[SP_STORY] = 0.f; }, true, BTN_PRIMARY);
        if (level == 2)
        {
            // §2.7 "Play from this beat (on the Outline, Beat and Cast pages)"
            const std::string beatId = sdAt(mDraftPack, mSel)["id"].asString();
            tool("Play from this beat", "Start Test Play here (only you; on the draft)", [this, beatId]() { mTestBeat = beatId; mPage = SP_TEST; testStart(beatId); }, !beatId.empty());
        }
        tool("Up", "Move it up", [this, arr, idxSel]() { docMove(0, arr, idxSel, -1); }, idxSel > 0);
        tool("Down", "Move it down", [this, arr, idxSel]() { docMove(0, arr, idxSel, 1); }, idxSel + 1 < count);
        tool("Duplicate", "Duplicate it (and everything in it)", [this, arr, idxSel]() { docDuplicate(0, arr, idxSel); }, true);
        if (level == 2)
        {
            const std::string sel = mSel;
            tool("Move to scene...", "Move this beat to another scene (the single-pointer alternative to dragging, §6 2.5.7)", [this, sel]()
            {
                std::vector<std::pair<std::string, std::string>> opts;
                const LLSD& acts2 = std::as_const(mDraftPack)["acts"];
                for (size_t a = 0; a < (size_t)acts2.size(); ++a)
                    for (size_t s2 = 0; s2 < (size_t)acts2[(LLSD::Integer)a]["scenes"].size(); ++s2)
                        opts.push_back({ llformat("/acts/%d/scenes/%d", (S32)a, (S32)s2), cap(acts2[(LLSD::Integer)a]["scenes"][(LLSD::Integer)s2]["title"].asString(), 60) });
                openPicker("Move the beat to", opts, [this, sel](const std::string& dest)
                {
                    if (dest.empty() || ptrParent(ptrParent(sel)) == dest) return;
                    const LLSD beat = sdAt(mDraftPack, sel);
                    const std::string dp = dest + "/beats";
                    if (!beat.isMap()) { setStatus(STALE, true); return; }
                    const bool moved = editDoc([&sel, &dp](boost::json::value& root2)
                    {
                        // taken out first (its pointer is still right), then added at the end of the other scene
                        boost::json::value* from = jAt(root2, sel, false);
                        boost::json::value* to = jAt(root2, dp, false);
                        if (!from || !from->is_object() || !to || !to->is_array()) return false;
                        const boost::json::value b = *from;
                        if (!jErase(root2, sel)) return false;
                        to = jAt(root2, dp, false);
                        if (!to || !to->is_array()) return false;
                        to->as_array().push_back(b);
                        return true;
                    });
                    if (!moved) { setStatus(STALE, true); return; }
                    stopPlacing();
                    mSel = ptrIndex(dp, (S32)sdAt(mDraftPack, dp).size() - 1);
                    setStatus("Moved. A part needs at least one box: the checks say so if one is left empty.", false);
                }, std::string());
            }, true);
        }
        const std::string what = level == 0 ? "this act and its scenes" : level == 1 ? "this scene and its beats" : "this beat";
        const std::string sel = mSel;
        tool("Delete", "Delete " + what + " (asks first)", [this, sel, what]() { askDelete(0, sel, what); }, true, BTN_DANGER);
    }
    y -= ROW_H + 10.f;
    // the flow strip (§2.2): the scene's beats as boxes, arrows for next / goto / decision options, dead ends marked
    std::string scPtr;
    if (level >= 1) scPtr = "/acts/" + t[1] + "/scenes/" + t[3];
    if (!scPtr.empty())
    {
        y = head(l, r, y, "Flow of this scene", p);
        const LLSD& beats = sdAt(mDraftPack, scPtr)["beats"];
        std::vector<std::string> scene_exits;
        for (const LLSD& ex : llsd::inArray(sdAt(mDraftPack, scPtr)["exits"]))
            scene_exits.push_back("scene " + ex["to"].asString() + (ex["label"].asString().empty() ? std::string() : " (\"" + cap(ex["label"].asString(), 32) + "\")")
                                  + (ex.has("if") ? " - only if..." : ""));
        if (!scene_exits.empty())
        {
            std::string line = "Exits of this scene: ";
            for (size_t i = 0; i < scene_exits.size(); ++i) line += (i ? ", " : "") + scene_exits[i];
            y = para(l, r, y, line, p.mDim, true) - 4.f;
        }
        for (size_t b = 0; beats.isArray() && b < (size_t)beats.size(); ++b)
        {
            const LLSD& bt = beats[(LLSD::Integer)b];
            std::vector<std::string> outs;
            auto scan = [&](const LLSD& effects, const std::string& via)
            {
                for (const LLSD& e : llsd::inArray(effects))
                {
                    if (e.has("goto")) outs.push_back("-> " + (e["goto"].has("beat") ? e["goto"]["beat"].asString() : "scene " + e["goto"]["scene"].asString()) + " (" + via + ")");
                    if (e.has("end")) outs.push_back("-> ending " + e["end"].asString() + " (" + via + ")");
                }
            };
            for (const LLSD& tr : llsd::inArray(bt["triggers"]))
            {
                if (tr.has("next")) outs.push_back("-> " + tr["next"].asString() + " (" + tr["on"].asString() + ": " + tr["id"].asString() + ")");
                scan(tr["do"], tr["on"].asString() + ": " + tr["id"].asString());
            }
            for (const LLSD& d : llsd::inArray(bt["decisions"]))
                for (const LLSD& o : llsd::inArray(d["options"])) scan(o["do"], "choice " + d["id"].asString() + " / " + o["id"].asString());
            scan(bt["onEnter"], "when it starts");
            scan(bt["recover"]["do"], "if they wander off");
            // the scene's exits lead on from any of its beats (ADVENTURE_PACK_V1.md scene exits)
            const bool dead = outs.empty() && scene_exits.empty();
            const F32 lh = (F32)small->getLineHeight();
            const F32 bh = 26.f + lh * (F32)llmax((size_t)1, outs.size());
            const std::string bp = ptrIndex(scPtr + "/beats", (S32)b);
            card(l, y - bh, r, y, p, 4.f);
            // a dead end: a red rim AND the words "dead end" (§2.2)
            if (dead) chamferEdge(l, y - bh, r, y, 4.f, 2.f, DANGER_INK);
            textL(bold, "Beat: " + cap(bt["objective"].asString(), 80), l + 8.f, y - 6.f, p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l - 16.f);
            F32 yy = y - 6.f - lh - 2.f;
            if (dead) textL(bold, "dead end - nothing leads on from here", l + 16.f, yy, DANGER_INK);
            for (const std::string& o : outs) { textL(small, o, l + 16.f, yy, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, r - l - 24.f); yy -= lh; }
            const S32 idx = act([this, bp]() { mSel = bp; mPage = SP_STORY; mScroll[SP_STORY] = 0.f; });
            hit(l, y - bh, r, y, SH_E, idx, "Open this beat on Advanced editing > Story");
            if (mKeyboardNav && hasFocus() && mHits.isFocused(SH_E, idx)) focusRing(l, y - bh, r, y, p);
            y -= bh + 4.f;
        }
        y -= 6.f;
    }
    // the adventure itself, and the endings
    const boost::json::value& as = def(root, "adventure");
    y = head(l, r, y, "The adventure", p);
    {
        // the region the owner stands on, one click (§6 3.3.7 Redundant Entry)
        LLViewerRegion* rgn = gAgent.getRegion();
        if (rgn)
        {
            const std::string rid = rgn->getRegionID().asString(), rname = rgn->getName();
            bool listed = false;
            for (const LLSD& rg : llsd::inArray(std::as_const(mDraftPack)["adventure"]["regions"])) listed = listed || rg["region"].asString() == rid;
            const S32 idx = act([this, rid, rname]()
            {
                docAppend(0, "/adventure/regions", boost::json::object{ { "region", rid }, { "name", cap(rname, 64) } });
                setStatus("Added " + rname + ". Request it on Advanced editing > Publish.", false);
            });
            btn(l, y - ROW_H, l + 260.f, y, "Add " + cap(rname, 24) + " (the region you are on)", SH_E, idx, "Add the region you are on to the adventure's regions", p, !listed);
            y -= ROW_H + 6.f;
        }
    }
    y = fObject(l, r, y, "/adventure", as, std::as_const(mDraftPack)["adventure"], 0, p, std::vector<std::string>(), { "learning" });
    const boost::json::value& ps = packSchema();
    y = fArray(l, r, y, "/endings", "endings", prop(ps, "endings"), std::as_const(mDraftPack)["endings"], 0, p, "Endings");
    return y;
}

namespace
{
    /** A beat of a parsed pack by id (acts[].scenes[].beats[]), or null. */
    boost::json::object* beatById(boost::json::value& root, const std::string& id)
    {
        if (id.empty() || !root.is_object()) return nullptr;
        boost::json::value* acts = root.as_object().if_contains("acts");
        if (!acts || !acts->is_array()) return nullptr;
        for (auto& a : acts->as_array())
        {
            boost::json::value* sc = a.is_object() ? a.as_object().if_contains("scenes") : nullptr;
            if (!sc || !sc->is_array()) continue;
            for (auto& s : sc->as_array())
            {
                boost::json::value* bs = s.is_object() ? s.as_object().if_contains("beats") : nullptr;
                if (!bs || !bs->is_array()) continue;
                for (auto& b : bs->as_array())
                    if (b.is_object() && b.as_object().contains("id") && b.as_object().at("id").is_string() && b.as_object().at("id").as_string() == id)
                        return &b.as_object();
            }
        }
        return nullptr;
    }
    boost::json::array& listMember(boost::json::object& o, const char* key)
    {
        boost::json::value& v = o[key];
        if (!v.is_array()) v = boost::json::array();
        return v.as_array();
    }
    /**
     * Wire the new beat `nb` in after `prev` (ADVENTURE_PACK_V1.md §11: a trigger's `next`, the effects `goto` and `end`):
     * prev's first way on (a trigger's next, else a trigger's goto, else a trigger's end step) is pointed at the new beat,
     * and the new beat takes over where it led with a "When everyone is ready" trigger (§11.1 `ready`, no fields) the
     * author then changes. A prev with no way on gets one to the new beat, and the new beat ends the story with the
     * pack's first ending (endings minItems 1). Returns the words for the status line.
     */
    std::string chainAfter(boost::json::object* prev, boost::json::object& nb, const std::string& firstEnding,
                           const std::function<std::string()>& newTriggerId)
    {
        const std::string nid(nb["id"].as_string().c_str());
        boost::json::array& nt = listMember(nb, "triggers");
        auto ready = [&](const char* k, const boost::json::value& v)
        {
            boost::json::object t{ { "id", newTriggerId() }, { "on", "ready" } };
            t[k] = v;
            nt.push_back(t);
        };
        if (!prev)
        {
            if (!firstEnding.empty()) ready("do", boost::json::array{ boost::json::object{ { "end", firstEnding } } });
            return "It is the story's first beat and, for now, ends the story.";
        }
        boost::json::array& pt = listMember(*prev, "triggers");
        for (auto& tv : pt)
        {
            if (!tv.is_object()) continue;
            boost::json::object& to = tv.as_object();
            if (to.contains("next") && to.at("next").is_string())
            {
                const boost::json::value old = to.at("next");
                to["next"] = nid;
                ready("next", old);
                return "It sits between the beat before it and the one that came next.";
            }
        }
        for (auto& tv : pt)
        {
            if (!tv.is_object()) continue;
            boost::json::value* dov = tv.as_object().if_contains("do");
            if (!dov || !dov->is_array()) continue;
            boost::json::array& d = dov->as_array();
            for (size_t i = 0; i < d.size(); ++i)
            {
                if (!d[i].is_object()) continue;
                boost::json::object& e = d[i].as_object();
                if (e.contains("goto") && e.at("goto").is_object())
                {
                    const boost::json::value old = e.at("goto");
                    e["goto"] = boost::json::object{ { "beat", nid } };
                    ready("do", boost::json::array{ boost::json::object{ { "goto", old } } });
                    return "It sits between the beat before it and where that beat went.";
                }
                if (e.contains("end"))
                {
                    const boost::json::value old = e.at("end");
                    d.erase(d.begin() + (std::ptrdiff_t)i);
                    tv.as_object()["next"] = nid;
                    ready("do", boost::json::array{ boost::json::object{ { "end", old } } });
                    return "The beat before it now leads here, and this beat ends the story.";
                }
            }
        }
        // [review] the beat before ends with the players' choice (its decisions lead on): a trigger round it would let them
        // skip the choice, so the author points an option here instead (the plain problem says so until they do)
        if (const boost::json::value* dv = prev->if_contains("decisions"))
            if (dv->is_array() && !dv->as_array().empty())
            {
                if (!firstEnding.empty()) ready("do", boost::json::array{ boost::json::object{ { "end", firstEnding } } });
                return "Nothing leads here yet: the beat before ends with a choice, so on Advanced editing > Story make one of its options go to this beat.";
            }
        pt.push_back(boost::json::object{ { "id", newTriggerId() }, { "on", "ready" }, { "next", nid } });
        if (!firstEnding.empty()) ready("do", boost::json::array{ boost::json::object{ { "end", firstEnding } } });
        return "The beat before it now leads here, and this beat ends the story.";
    }
}

void WolfAdventureStudioView::addStoryItem(S32 what, const std::string& sel)
{
    // what: 0 an act at the end, 1 a scene at the end of the selected act, 2 a beat after the selected beat (or at the end of
    // the selected scene). The new beat (an act's or scene's own first one) follows the beat before it in pack order.
    const boost::json::value& root = packSchema();
    const LLSD& pk = mDraftPack;
    const std::vector<std::string> t = ptrTokens(sel);
    const S32 sa = t.size() >= 2 ? tokIndex(t[1]) : -1, ss = t.size() >= 4 ? tokIndex(t[3]) : -1, sb = t.size() >= 6 ? tokIndex(t[5]) : -1;
    const std::string region = std::as_const(mDraftPack)["adventure"]["regions"][0]["region"].asString();
    // the last beat at or before (act a, scene s, beat b) in pack order (b = -1: before scene s's beats; s = -1: before act a)
    auto lastBefore = [&pk](S32 a, S32 s, S32 b) -> std::string
    {
        std::string last;
        const LLSD& acts = pk["acts"];
        for (S32 ai = 0; ai < (S32)acts.size(); ++ai)
            for (S32 si = 0; si < (S32)acts[ai]["scenes"].size(); ++si)
                for (S32 bi = 0; bi < (S32)acts[ai]["scenes"][si]["beats"].size(); ++bi)
                {
                    const bool before = ai < a || (ai == a && (si < s || (si == s && bi <= b)));
                    if (before) last = acts[ai]["scenes"][si]["beats"][bi]["id"].asString();
                }
        return last;
    };
    std::string arrPtr, prev;
    S32 at = 0;
    boost::json::value item;
    const S32 nacts = pk["acts"].isArray() ? (S32)pk["acts"].size() : 0;
    if (what == 0)
    {
        arrPtr = "/acts";
        at = nacts;
        prev = lastBefore(nacts, 0, -1);
        item = makeDefault(root, boost::json::object{ { "$ref", "#/$defs/act" } }, "acts", mDraftJson, region, at);
    }
    else if (what == 1)
    {
        if (sa < 0 || sa >= nacts) { setStatus(STALE, true); return; }
        arrPtr = llformat("/acts/%d/scenes", sa);
        at = (S32)pk["acts"][sa]["scenes"].size();
        prev = lastBefore(sa, at, -1);
        item = makeDefault(root, boost::json::object{ { "$ref", "#/$defs/scene" } }, "scenes", mDraftJson, region, at);
    }
    else
    {
        if (sa < 0 || ss < 0 || !pk["acts"][sa]["scenes"][ss].isMap()) { setStatus(STALE, true); return; }
        arrPtr = llformat("/acts/%d/scenes/%d/beats", sa, ss);
        const S32 nb = (S32)pk["acts"][sa]["scenes"][ss]["beats"].size();
        at = sb >= 0 ? llmin(sb + 1, nb) : nb;
        prev = lastBefore(sa, ss, at - 1);
        item = makeDefault(root, boost::json::object{ { "$ref", "#/$defs/beat" } }, "beats", mDraftJson, region, at);
    }
    if (!item.is_object()) return;
    // the new beat's id (an act's first scene's first beat, a scene's first beat, or the beat itself)
    std::string nid;
    {
        const boost::json::value* b = &item;
        if (what == 0) b = jAt(item, "/scenes/0/beats/0", false);
        else if (what == 1) b = jAt(item, "/beats/0", false);
        if (b && b->is_object() && b->as_object().contains("id")) nid = std::string(b->as_object().at("id").as_string().c_str());
    }
    std::string seen = mDraftJson + boost::json::serialize(item);
    auto newTrigger = [&seen]() { const std::string n = uniqueId("trigger", seen); seen += "\"id\":\"" + n + "\""; return n; };
    const std::string ending = pk["endings"][0]["id"].asString();
    std::string said;
    const bool ok = editDoc([&](boost::json::value& doc)
    {
        boost::json::value* arr = jAt(doc, arrPtr, true);
        if (!asList(arr)) return false;
        boost::json::array& a = arr->as_array();
        a.insert(a.begin() + llclamp(at, 0, (S32)a.size()), item);
        boost::json::object* nb = beatById(doc, nid);
        if (!nb) return true;
        said = chainAfter(beatById(doc, prev), *nb, ending, newTrigger);
        // the story's start names no beat that exists (an empty draft): it starts here
        boost::json::value* sb2 = jAt(doc, "/adventure/start/beat", false);
        const std::string start = sb2 && sb2->is_string() ? std::string(sb2->as_string().c_str()) : std::string();
        if (!beatById(doc, start)) { if (boost::json::value* st = jAt(doc, "/adventure/start/beat", true)) *st = nid; }
        return true;
    });
    if (!ok) { if (!mDraftJson.empty()) setStatus(STALE, true); return; }
    // select what was made (its own pointer), so the next Add goes after it
    mSel = ptrIndex(arrPtr, at);
    static const char* WHAT[3] = { "Added an act.", "Added a scene.", "Added a beat." };
    setStatus(std::string(WHAT[llclamp(what, 0, 2)]) + " " + said + " Give it a title and objective, and choose what finishes it (it starts as \"When everyone is ready\", which only a party can press), on Advanced editing > Story (Open).", false);
}

F32 WolfAdventureStudioView::pageStory(F32 l, F32 r, F32 top, const Palette& p)
{
    if (!openIsOwner()) return waiting(l, r, top, "Only the adventure's owner edits its story.", p);
    if (mDraftJson.empty()) return waiting(l, r, top, "Opening the draft...", p);
    const boost::json::value& root = packSchema();
    // the node to edit: the Outline's selection (a beat by default)
    if (mSel.empty() || !sdAt(mDraftPack, mSel).isMap())
    {
        mSel.clear();
        if (std::as_const(mDraftPack)["acts"][0]["scenes"][0]["beats"][0].isMap()) mSel = "/acts/0/scenes/0/beats/0";
    }
    if (mSel.empty()) return waiting(l, r, top, "Add an act, a scene and a beat on the Outline page first.", p);
    const std::vector<std::string> t = ptrTokens(mSel);
    const S32 level = t.size() >= 6 ? 2 : t.size() >= 4 ? 1 : 0;
    F32 y = conflictBanner(l, r, top, p);
    // where we are, and a way to move (the beats in pack order, the single-pointer route)
    {
        std::string crumb = "Act " + llformat("%d", tokIndex(t[1]) + 1) + ": " + cap(sdAt(mDraftPack, "/acts/" + t[1])["title"].asString(), 40);
        if (level >= 1) crumb += "  >  Scene: " + cap(sdAt(mDraftPack, "/acts/" + t[1] + "/scenes/" + t[3])["title"].asString(), 40);
        if (level >= 2) crumb += "  >  Beat " + llformat("%d", tokIndex(t[5]) + 1);
        textL(LLFontGL::getFontSansSerifSmallBold(), crumb, l, y - 4.f, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, r - l - 260.f);
        const S32 ip = act([this]()
        {
            std::vector<std::pair<std::string, std::string>> opts;
            const LLSD& acts = std::as_const(mDraftPack)["acts"];
            for (size_t a = 0; a < (size_t)acts.size(); ++a)
            {
                opts.push_back({ llformat("/acts/%d", (S32)a), "Act: " + cap(acts[(LLSD::Integer)a]["title"].asString(), 50) });
                for (size_t s = 0; s < (size_t)acts[(LLSD::Integer)a]["scenes"].size(); ++s)
                {
                    const LLSD& sc = acts[(LLSD::Integer)a]["scenes"][(LLSD::Integer)s];
                    opts.push_back({ llformat("/acts/%d/scenes/%d", (S32)a, (S32)s), "   Scene: " + cap(sc["title"].asString(), 50) });
                    for (size_t b = 0; b < (size_t)sc["beats"].size(); ++b)
                        opts.push_back({ llformat("/acts/%d/scenes/%d/beats/%d", (S32)a, (S32)s, (S32)b), "      Beat: " + cap(sc["beats"][(LLSD::Integer)b]["objective"].asString(), 50) });
                }
            }
            openPicker("Edit", opts, [this](const std::string& v) { if (!v.empty()) { mSel = v; mScroll[SP_STORY] = 0.f; } }, std::string());
        });
        btn(r - 250.f, y - ROW_H, r - 120.f, y, "Go to...", SH_E, ip, "Choose the act, scene or beat to edit", p);
        const S32 io = act([this]() { mPage = SP_OUTLINE; });
        btn(r - 116.f, y - ROW_H, r, y, "Outline", SH_E, io, "Back to the Outline", p);
        y -= ROW_H + 8.f;
    }
    const LLSD& node = sdAt(mDraftPack, mSel);
    if (level == 0)
    {
        y = head(l, r, y, "Act", p);
        return fObject(l, r, y, mSel, def(root, "act"), node, 0, p, std::vector<std::string>(), { "scenes" });
    }
    if (level == 1)
    {
        const boost::json::value& ss = def(root, "scene");
        y = head(l, r, y, "Scene", p);
        y = fObject(l, r, y, mSel, ss, node, 0, p, { "id", "title" });
        y = head(l, r, y, "Where", p);
        y = fObject(l, r, y, mSel, ss, node, 0, p, { "region", "area", "shared" });
        y = fValue(l, r, y, ptrJoin(mSel, "stage"), "stage", prop(ss, "stage"), node["stage"], true, 0, p, "Stage");
        y = fObject(l, r, y, mSel, ss, node, 0, p, { "boxed", "gmNotes", "handouts", "entry", "onEnter", "exits", "learning" });
        return y;
    }
    const boost::json::value& bs = def(root, "beat");
    y = head(l, r, y, "Beat", p);
    y = fObject(l, r, y, mSel, bs, node, 0, p, { "id", "title", "objective", "marker" });
    y = fValue(l, r, y, ptrJoin(mSel, "onDuty"), "onDuty", prop(bs, "onDuty"), node["onDuty"], true, 0, p, "On duty");
    y = fEffects(l, r, y, ptrJoin(mSel, "onEnter"), node["onEnter"], p, "When this beat starts");
    y = fTriggers(l, r, y, ptrJoin(mSel, "triggers"), node["triggers"], p);
    y = fObject(l, r, y, mSel, bs, node, 0, p, { "decisions", "hints", "recover", "aiNotes" });
    // play from here (§2.7 "Play from this beat" on the Outline, Beat and Cast pages)
    const std::string beatId = node["id"].asString();
    const S32 it = act([this, beatId]() { mTestBeat = beatId; mPage = SP_TEST; testStart(beatId); });
    btn(l, y - ROW_H, l + 200.f, y, "Play from this beat", SH_E, it, "Start Test Play here (only you; on the draft)", p, true, BTN_PRIMARY);
    return y - ROW_H - 8.f;
}

std::vector<std::string> WolfAdventureStudioView::castUsesList(const std::string& cid) const
{
    // every place a character is named (ADVENTURE_PACK_V1.md §15 cross-references), apart from its own sheet
    std::vector<std::string> uses;
    std::function<void(const LLSD&, const std::string&)> walk = [&](const LLSD& v, const std::string& ptr)
    {
        if (ptr.rfind("/cast/", 0) == 0 && ptrTokens(ptr).size() >= 2 && ptrTokens(ptr)[0] == "cast") return;
        if (v.isMap())
        {
            for (const auto& kv : llsd::inMap(v))
            {
                if ((kv.first == "cast" || kv.first == "by" || kv.first == "castRep") && kv.second.isString() && kv.second.asString() == cid) uses.push_back(ptr);
                else if (kv.first == "dismiss" && kv.second.isArray())
                {
                    for (const LLSD& x : llsd::inArray(kv.second)) if (x.asString() == cid) uses.push_back(ptr);
                }
                else walk(kv.second, ptrJoin(ptr, kv.first));
            }
        }
        else if (v.isArray()) for (size_t i = 0; i < (size_t)v.size(); ++i) walk(v[(LLSD::Integer)i], ptrIndex(ptr, (S32)i));
    };
    for (const auto& kv : llsd::inMap(mDraftPack)) if (kv.first != "cast") walk(kv.second, "/" + kv.first);
    return uses;
}

std::string WolfAdventureStudioView::castUses(const std::string& cid) const
{
    const std::vector<std::string> uses = castUsesList(cid);
    if (uses.empty()) return std::string();
    std::string s = llformat("used in %d place%s: ", (S32)uses.size(), uses.size() == 1 ? "" : "s");
    for (size_t i = 0; i < uses.size() && i < 4; ++i) s += (i ? ", " : "") + uses[i];
    return s;
}

F32 WolfAdventureStudioView::pageCastEd(F32 l, F32 r, F32 top, const Palette& p)
{
    if (!openIsOwner()) return waiting(l, r, top, "Only the adventure's owner edits the cast.", p);
    if (mDraftJson.empty()) return waiting(l, r, top, "Opening the draft...", p);
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const boost::json::value& root = packSchema();
    const boost::json::value& cs = def(root, "castSheet");
    const LLSD& cast = std::as_const(mDraftPack)["cast"];
    const S32 n = cast.isArray() ? (S32)cast.size() : 0;
    mCastSel2 = llclamp(mCastSel2, 0, llmax(0, n - 1));
    F32 y = head(l, r, conflictBanner(l, r, top, p), "Characters", p);
    for (S32 i = 0; i < n; ++i)
    {
        const LLSD& c = cast[i];
        const std::string cp = ptrIndex("/cast", i);
        const bool sel = i == mCastSel2;
        if (sel) rect(l, y - 26.f, r, y, fade(p.mAccent, 0.22f));
        const S32 idx = act([this, i]() { mCastSel2 = i; mScroll[SP_CASTED] = 0.f; });
        std::string text = cap(c["first"].asString() + " " + c["last"].asString(), 40) + "  -  " + cap(c["role"].asString(), 80);
        const S32 np = problemsUnder(cp);
        if (np) text += llformat("  (%d problems)", np);
        textL(sel ? LLFontGL::getFontSansSerifSmallBold() : small, text, l + 6.f, y - 13.f, sel ? p.mAccent : (np ? WARN_INK : p.mText), LLFontGL::LEFT, LLFontGL::VCENTER, r - l - 12.f);
        hit(l, y - 26.f, r, y, SH_E, idx, "Edit " + cap(c["first"].asString() + " " + c["last"].asString(), 40));
        if (mKeyboardNav && hasFocus() && mHits.isFocused(SH_E, idx)) focusRing(l, y - 26.f, r, y, p);
        y -= 28.f;
    }
    {
        F32 x = l;
        const S32 ia = act([this]() { addCastMember(); });
        btn(x, y - ROW_H, x + 90.f, y, "Add", SH_E, ia, "Add a character", p, n < 100);
        x += 94.f;
        const S32 id = act([this]() { docDuplicate(0, "/cast", mCastSel2); });
        btn(x, y - ROW_H, x + 100.f, y, "Duplicate", SH_E, id, "Duplicate this character", p, n > 0);
        x += 104.f;
        const std::string cid = n > 0 ? cast[mCastSel2]["id"].asString() : std::string();
        const std::string cp = ptrIndex("/cast", mCastSel2);
        const std::string nm = n > 0 ? cap(cast[mCastSel2]["first"].asString() + " " + cast[mCastSel2]["last"].asString(), 40) : std::string();
        const S32 ix = act([this, cp, cid, nm]()
        {
            // §2.3 "Delete (refused while a beat or stage uses the character; the list of uses is shown)"
            const std::string uses = castUses(cid);
            if (!uses.empty()) { setStatus(nm + " cannot be deleted while the story uses them - " + uses, true); return; }
            askDelete(0, cp, nm);
        });
        btn(x, y - ROW_H, x + 90.f, y, "Delete", SH_E, ix, "Delete this character (asks first; refused while the story uses them, with the list)", p, n > 0, BTN_DANGER);
        x += 94.f;
        const S32 iv = act([this]() { mPage = SP_CAST; mCastSel = mCastSel2; loadVoices(); loadBudget(); });
        btn(x, y - ROW_H, x + 160.f, y, "Talk to them now...", SH_E, iv, "Try this character's talk on any beat (Cast talk page)", p, n > 0);
        y -= ROW_H + 10.f;
    }
    if (n == 0) return para(l, r, y, "No characters yet: Add one.", p.mDim);
    const std::string cp = ptrIndex("/cast", mCastSel2);
    const LLSD& c = cast[mCastSel2];
    // Name and look (§2.3)
    y = head(l, r, y, "Name and look", p);
    y = fObject(l, r, y, cp, cs, c, 0, p, { "id", "first", "last", "title", "portrait" });
    y = fValue(l, r, y, ptrJoin(cp, "look"), "look", prop(cs, "look"), c["look"], true, 0, p, "Look");
    {
        const std::string cid = c["id"].asString();
        const S32 iw = act([this, cid]() { saveLook(cid); });
        btn(l + LABEL_W, y - ROW_H, l + LABEL_W + 200.f, y, "Use what I'm wearing", SH_E, iw,
            "Save your current outfit as this character's appearance (you must be on a region this adventure is enabled on)", p, mLookCast.empty());
        if (!mLookCast.empty()) textL(small, "Saving what you are wearing...", l + LABEL_W + 210.f, y - ROW_H * 0.5f, p.mDim, LLFontGL::LEFT, LLFontGL::VCENTER);
        y -= ROW_H + 8.f;
    }
    // Who they are
    y = head(l, r, y, "Who they are", p);
    y = fObject(l, r, y, cp, cs, c, 0, p, { "role", "persona", "manner", "greeting", "home" });
    // What they know - secrets with a lock and the words "never revealed"
    y = head(l, r, y, "What they know", p);
    y = fValue(l, r, y, ptrJoin(cp, "knows"), "knows", prop(cs, "knows"), c["knows"], true, 0, p);
    {
        lineW(l + 2.f, y - 10.f, l + 2.f, y - 4.f, 2.f, WARN_INK);   // a small lock: shackle and body
        rect(l - 2.f, y - 16.f, l + 6.f, y - 10.f, WARN_INK);
        y = fValue(l + 10.f, r, y, ptrJoin(cp, "neverReveal"), "neverReveal", prop(cs, "neverReveal"), c["neverReveal"], true, 0, p, "Secrets - never revealed");
    }
    y = fValue(l, r, y, ptrJoin(cp, "neverSay"), "neverSay", prop(cs, "neverSay"), c["neverSay"], true, 0, p);
    y = fValue(l, r, y, ptrJoin(cp, "cards"), "cards", prop(cs, "cards"), c["cards"], true, 0, p);
    // Lines
    y = head(l, r, y, "Lines", p);
    y = fObject(l, r, y, cp, cs, c, 0, p, { "idle", "barks" });
    // Voice: the voice list from the brain (WOLF_MIND §8.2), premium voices in their own group (off by default)
    y = head(l, r, y, "Voice", p);
    y = voiceRow(l, r, y, mCastSel2, true, p);
    // Day
    y = scheduleRibbon(l, r, y, ptrJoin(cp, "schedule"), c["schedule"], p);
    // May do: the allowed actions with a line each (§2.3), and the AI triggers
    y = head(l, r, y, "May do", p);
    {
        static const char* ACTS[][2] = {
            { "give_hint", "Give the player a hint" }, { "offer_choice", "Offer one of the beat's decisions" },
            { "fire_trigger", "Make a story moment happen (the AI triggers below)" }, { "refuse", "Refuse what the player asks" },
            { "call_gm", "Call a host (a person)" }, { "follow", "Start following the player" }, { "stop_following", "Stop following" },
            { "attack", "Turn hostile and fight (needs Fights)" }, { "flee", "Run away home" }, { "end_talk", "End the conversation" } };
        const LLSD& cur = c["actions"];
        for (const auto& a : ACTS)
        {
            bool on = false;
            for (const LLSD& x : llsd::inArray(cur)) on = on || x.asString() == a[0];
            const std::string ap = ptrJoin(cp, "actions");
            const std::string key = a[0];
            const S32 idx = act([this, ap, key, on, cur]()
            {
                boost::json::array out;
                for (const LLSD& x : llsd::inArray(cur)) if (x.asString() != key) out.push_back(boost::json::value(x.asString()));
                if (!on) out.push_back(boost::json::value(key));
                docSet(0, ap, out);
            });
            // a check box: a box with a tick AND the words On / Off in the tip (colour never alone)
            chamferEdge(l, y - 18.f, l + 18.f, y, 2.f, 1.5f, p.mText);
            if (on) { lineW(l + 4.f, y - 9.f, l + 8.f, y - 14.f, 2.f, OK_INK); lineW(l + 8.f, y - 14.f, l + 15.f, y - 4.f, 2.f, OK_INK); }
            textL(small, a[1], l + 26.f, y - 9.f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, r - l - 30.f);
            hit(l, y - 20.f, r, y, SH_E, idx, std::string(a[1]) + (on ? " - allowed now" : " - not allowed now"));
            if (mKeyboardNav && hasFocus() && mHits.isFocused(SH_E, idx)) focusRing(l, y - 20.f, r, y, p);
            y -= 24.f;
        }
        y -= 4.f;
        y = fValue(l, r, y, ptrJoin(cp, "triggerIds"), "triggerIds", prop(cs, "triggerIds"), c["triggerIds"], true, 0, p);
    }
    // Fights
    y = head(l, r, y, "Fights", p);
    if (!c["actor"].isMap())
    {
        const std::string ap = ptrJoin(cp, "actor");
        const S32 idx = act([this, ap]() { docSet(0, ap, boost::json::object{ { "health", 100 } }); });
        btn(l, y - ROW_H, l + 200.f, y, "Make them a fighter", SH_E, idx, "Make this character a wolfGame actor (targetable, with health)", p);
        y -= ROW_H + 8.f;
    }
    else
    {
        y = fObject(l, r, y, ptrJoin(cp, "actor"), prop(cs, "actor"), c["actor"], 0, p);
        const std::string ap = ptrJoin(cp, "actor");
        const S32 idx = act([this, ap]() { docErase(0, ap); });
        btn(l, y - ROW_H, l + 200.f, y, "Not a fighter", SH_E, idx, "Remove the fight settings", p, true, BTN_DANGER);
        y -= ROW_H + 8.f;
    }
    // Play from a beat where they are on duty (§2.7 "Play from this beat (on the Outline, Beat and Cast pages)")
    {
        std::string beat;
        const std::string cid = c["id"].asString();
        for (const LLSD& a : llsd::inArray(std::as_const(mDraftPack)["acts"])) for (const LLSD& sc : llsd::inArray(a["scenes"])) for (const LLSD& b : llsd::inArray(sc["beats"]))
            for (const LLSD& od : llsd::inArray(b["onDuty"])) if (beat.empty() && od["cast"].asString() == cid) beat = b["id"].asString();
        const S32 it = act([this, beat]() { mTestBeat = beat; mPage = SP_TEST; testStart(beat); });
        btn(l, y - ROW_H, l + 260.f, y, beat.empty() ? std::string("Not on duty in any beat yet") : "Play from their first beat (" + beat + ")", SH_E, it,
            "Start Test Play where this character is on duty", p, !beat.empty(), BTN_PRIMARY);
        y -= ROW_H + 8.f;
    }
    return y;
}

F32 WolfAdventureStudioView::voiceRow(F32 l, F32 r, F32 top, S32 ci, bool rate, const Palette& p)
{
    // [SIMPLE STUDIO 2026-10-17] the voice picker of one character, for the Cast page and page 2 (written once)
    const LLSD& cast = std::as_const(mDraftPack)["cast"];
    if (!cast.isArray() || ci < 0 || ci >= (S32)cast.size()) return top;
    const LLSD& c = cast[ci];
    const std::string cp = ptrIndex("/cast", ci);
    const boost::json::value& cs = def(packSchema(), "castSheet");
    F32 y = top;
    if (!mVoicesLoaded) loadVoices();
    F32 cl = l;
    fRow(l, r, y, "Voice", cl);
    const std::string cur = c["voice"]["id"].asString();
    std::string shown = cur.empty() ? std::string("(none - text only)") : cur;
    for (const LLSD& vo : llsd::inArray(mVoices)) if (vo["id"].asString() == cur) shown = cap(vo["label"].asString(), 60) + (vo["premium"].asBoolean() ? " (premium)" : "");
    const std::string vp = ptrJoin(cp, "voice");
    bool cur_premium = false, any_premium = false;
    for (const LLSD& vo : llsd::inArray(mVoices))
    {
        if (!vo["premium"].asBoolean()) continue;
        any_premium = true;
        cur_premium = cur_premium || vo["id"].asString() == cur;
    }
    const bool show_premium = mShowPremium || cur_premium;
    const S32 iv = act([this, ci, show_premium]()
    {
        std::vector<std::pair<std::string, std::string>> opts;
        opts.push_back({ "~none", "(none - text only)" });
        for (const LLSD& vo : llsd::inArray(mVoices))
            if (!vo["premium"].asBoolean()) opts.push_back({ vo["id"].asString(), cap(vo["label"].asString(), 60) + " - free" });
        // premium voices only when their group is switched on (off by default, as on Cast talk)
        bool hdr = false;
        for (const LLSD& vo : llsd::inArray(mVoices))
        {
            if (!show_premium || !vo["premium"].asBoolean()) continue;
            if (!hdr) { opts.push_back({ "~hdr", "Premium voice (ElevenLabs) - charged per character spoken" }); hdr = true; }
            // credits = micro / 10000 (WOLF_MIND §7.1); per 100 characters
            const F64 per100 = vo["price"]["microPerChar"].asReal() * 100.0 / 10000.0;
            opts.push_back({ vo["id"].asString(), "   " + cap(vo["label"].asString(), 50) + llformat(" - %.2f credits per 100 characters", per100) });
        }
        openPicker("Voice", opts, [this, ci](const std::string& id)
        {
            if (id.empty() || id == "~hdr") return;
            pickVoice(ci, id == "~none" ? std::string() : id);
        }, "The voice list has not loaded - try again in a moment.");
    });
    const F32 tw = any_premium ? 250.f : 0.f;
    btn(cl, y - ROW_H, r - tw - (tw > 0.f ? 6.f : 0.f), y, shown, SH_E, iv, "Choose this character's voice (free piper voices cost nothing)", p);
    if (any_premium)
    {
        const S32 it = act([this]() { mShowPremium = !mShowPremium; });
        btn(r - tw, y - ROW_H, r, y, show_premium ? "Premium voices - hide" : "Premium voices (ElevenLabs) - show", SH_E, it,
            "Premium voices are charged per character spoken, from your credits", p, !cur_premium);
    }
    y -= ROW_H + 6.f;
    if (rate && c["voice"].isMap()) y = fValue(l, r, y, ptrJoin(vp, "rate"), "rate", prop(prop(cs, "voice"), "rate"), c["voice"]["rate"], true, 0, p, "Speed (0.5 to 2)");
    return y;
}

void WolfAdventureStudioView::addCastMember()
{
    const S32 n = std::as_const(mDraftPack)["cast"].isArray() ? (S32)std::as_const(mDraftPack)["cast"].size() : 0;
    if (n >= 100) { setStatus("A game holds at most 100 characters.", true); return; }
    const boost::json::value& root = packSchema();
    const std::string region = std::as_const(mDraftPack)["adventure"]["regions"][0]["region"].asString();
    docAppend(0, "/cast", makeDefault(root, def(root, "castSheet"), "cast", mDraftJson, region));
    mCastSel2 = n;
    // [CHARACTER GALLERY 2026-10-17] on page 2 the new one's page opens (editDoc has already put them into mDraftPack)
    const LLSD& cast = std::as_const(mDraftPack)["cast"];
    if (mPage == SP_CHARS && cast.isArray() && (S32)cast.size() == n + 1)
    {
        mCharsWho = "c:" + cast[n]["id"].asString();
        mScroll[SP_CHARS] = 0.f;
    }
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// [CHARACTER GALLERY 2026-10-17] page 2 "Your characters": a gallery of picture cards, and one character's page
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// Paul: "the characters page is a mess - I would rather have pictures for each character, then you click through to work with
// that character". The gallery shows one card per character (the enemies under their own head): a square picture, the name,
// one line (their role) and a look badge. A card opens that one's page: the big picture with Set picture..., and the fields
// of the old card in the old order (First name, Last name, Who they are, How they think and talk, First thing they say, What
// they never tell), the look row, the voice row, Hear them, Remove. Everything saves as before (fValue on /cast/N/...; the
// portrait through chooseTexture, as the cover); look_pad, look_pads_clear, owner_settings, save_look and previewVoice are
// the same calls. The 8-second pad re-read (padsTick) runs on both the gallery and the page (mPage stays SP_CHARS).

namespace
{
    const F32 CARD_W = 170.f;      // a gallery card: the picture (150) with 10 px each side
    const F32 CARD_PIC = 150.f;    // the card's picture, square
    const F32 CARD_GAP = 14.f;
    const F32 DETAIL_PIC = 200.f;  // the page's big picture, square

    /** The first letter of each of up to two words, upper-cased ("Jesse James" -> "JJ"; "" -> "?"). */
    std::string initialsOf(const std::string& a, const std::string& b)
    {
        std::string out;
        for (const std::string* w : { &a, &b })
        {
            for (unsigned char ch : *w)
            {
                if (isalnum(ch)) { out += (char)toupper(ch); break; }
            }
        }
        return out.empty() ? std::string("?") : out;
    }
}

// static
LLColor4 WolfAdventureStudioView::idColour(const std::string& id)
{
    // FNV-1a 32-bit (Fowler-Noll-Vo: offset 2166136261, prime 16777619) over the id's bytes picks the hue, so the same
    // character gets the same colour in every session and on both viewers (the web page uses the same hash)
    U32 h = 2166136261u;
    for (unsigned char ch : id) { h ^= ch; h *= 16777619u; }
    const F32 hue = (F32)(h % 360u);
    // HSL(hue, 45 %, 28 %) -> RGB (the standard conversion: C = (1 - |2L - 1|) S, X = C (1 - |(H / 60) mod 2 - 1|), m = L - C / 2).
    // 28 % lightness at 45 % saturation keeps the plate dark enough for the palettes' light text at every hue: the worst case,
    // hue 60 = rgb(104, 104, 39), relative luminance 0.13, is 4.9:1 against #e6ebf5 and 5.2:1 against #f2f2f2.
    const F32 S = 0.45f, L = 0.28f;
    const F32 C = (1.f - fabsf(2.f * L - 1.f)) * S;
    const F32 hp = hue / 60.f;
    const F32 X = C * (1.f - fabsf(fmodf(hp, 2.f) - 1.f));
    const F32 m = L - C * 0.5f;
    F32 r = 0.f, g = 0.f, b = 0.f;
    if (hp < 1.f) { r = C; g = X; }
    else if (hp < 2.f) { r = X; g = C; }
    else if (hp < 3.f) { g = C; b = X; }
    else if (hp < 4.f) { g = X; b = C; }
    else if (hp < 5.f) { r = X; b = C; }
    else { r = C; b = X; }
    return LLColor4(r + m, g + m, b + m, 1.f);
}

bool WolfAdventureStudioView::hasBoxes() const
{
    // a box is a beat: /acts/a/scenes/s/beats/b (adventure_pack_v1.schema.json "acts" :112 > "scenes" :1965 > "beats" :2053; the
    // flowchart's nodes). Read from the pack itself so the tabs are right before flowLayout has run on this draft.
    for (const LLSD& a : llsd::inArray(std::as_const(mDraftPack)["acts"]))
        for (const LLSD& sc : llsd::inArray(a["scenes"]))
            if (sc["beats"].isArray() && sc["beats"].size() > 0) return true;
    return false;
}

std::string WolfAdventureStudioView::charsPortraitId(const LLSD& sheet, bool enemy) const
{
    // Source: adventure_pack_v1.schema.json $defs castSheet "portrait": uuidOrEmpty (the cast sheet's picture); $defs enemy
    // "look": kind npc | cast | object, with "cast" naming the character the enemy looks like - their portrait is used then
    if (!enemy) return sheet["portrait"].asString();
    if (sheet["look"]["kind"].asString() != "cast") return std::string();
    const std::string cid = sheet["look"]["cast"].asString();
    for (const LLSD& c : llsd::inArray(std::as_const(mDraftPack)["cast"])) if (c["id"].asString() == cid) return c["portrait"].asString();
    return std::string();
}

std::string WolfAdventureStudioView::enemyLookWords(const LLSD& e) const
{
    const std::string kind = e["look"]["kind"].asString();
    if (kind == "cast")
    {
        const std::string cid = e["look"]["cast"].asString();
        for (const LLSD& c : llsd::inArray(std::as_const(mDraftPack)["cast"]))
            if (c["id"].asString() == cid) return "Looks like " + cap(c["first"].asString() + " " + c["last"].asString(), 40);
        return "Looks like a character who is no longer in the story";
    }
    if (kind == "object") return "An object from a chest";
    return lookWords(e);   // an NPC: look.standard / look.appearance / look.dressedBy, as a character
}

void WolfAdventureStudioView::charsPortrait(F32 l, F32 b, F32 sz, const LLSD& sheet, bool enemy, const Palette& p)
{
    const std::string cur = charsPortraitId(sheet, enemy);
    LLUUID id;
    const bool has = !cur.empty() && LLUUID::validate(cur) && id.set(cur, false) && id.notNull();
    if (has)
    {
        // Source: fTexture (this file) - a portrait is drawn from the texture fetch like any UI image
        chamfer(l, b, l + sz, b + sz, 4.f, p.mCard, p.mCard);
        LLViewerFetchedTexture* tex = LLViewerTextureManager::getFetchedTexture(id, FTT_DEFAULT, MIPMAP_YES, LLGLTexture::BOOST_UI);
        if (tex)
        {
            tex->addTextureStats((F32)(sz * sz));
            gl_draw_scaled_image((S32)l + 2, (S32)b + 2, (S32)sz - 4, (S32)sz - 4, tex);
        }
        chamferEdge(l, b, l + sz, b + sz, 4.f, 1.f, p.mDim);
        return;
    }
    // no picture: a plate in the colour of their id, a silhouette outline (head and shoulders) and their initials, large
    const LLColor4 plate = idColour(sheet["id"].asString());
    chamfer(l, b, l + sz, b + sz, 4.f, plate, plate);
    const LLColor4 line = fade(p.mText, 0.28f);
    const F32 cx = l + sz * 0.5f;
    arc(cx, b + sz * 0.68f, sz * 0.15f, 1.5f, 0.f, F_TWO_PI, line);
    chamferEdge(l + sz * 0.2f, b + sz * 0.06f, l + sz * 0.8f, b + sz * 0.46f, sz * 0.14f, 1.5f, line);
    const std::string ini = enemy ? initialsOf(sheet["name"].asString(), std::string()) : initialsOf(sheet["first"].asString(), sheet["last"].asString());
    textL(display(sz * 0.4f), ini, cx, b + sz * 0.5f, p.mText, LLFontGL::HCENTER, LLFontGL::VCENTER);
    chamferEdge(l, b, l + sz, b + sz, 4.f, 1.f, p.mDim);
}

F32 WolfAdventureStudioView::charsCard(F32 l, F32 top, F32 cw, const LLSD& sheet, bool enemy, const Palette& p)
{
    const LLFontGL* bold = LLFontGL::getFontSansSerifBold();
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const F32 lh = (F32)bold->getLineHeight(), sh = (F32)small->getLineHeight();
    const F32 ch = 8.f + CARD_PIC + 6.f + lh + 2.f + sh + 6.f + 20.f + 8.f;   // picture, name, one line, badge
    const F32 b = top - ch, r = l + cw;
    const std::string id = sheet["id"].asString();
    const std::string name = enemy ? cap(sheet["name"].asString(), 32) : cap(sheet["first"].asString() + " " + sheet["last"].asString(), 40);
    const std::string shown = name.size() > 1 ? name : std::string(enemy ? "New enemy" : "New character");
    // one line under the name: their role (the cast sheet's role; an enemy's first line of description, else "Enemy")
    std::string line = enemy ? WolfGame::cleanText(sheet["description"].asString(), 80) : WolfGame::cleanText(sheet["role"].asString(), 80);
    if (enemy && line.empty()) line = "Enemy";
    // the look badge: words and colour (never colour alone) from look.standard / look.appearance / look.dressedBy
    const std::string words = enemy ? enemyLookWords(sheet) : lookWords(sheet);
    const bool dressed = words.rfind("Dressed", 0) == 0;
    std::string badge = dressed ? "Dressed" : "Standard look";
    if (enemy && words.rfind("Looks like", 0) == 0) badge = "Looks like a character";
    else if (enemy && words.rfind("An object", 0) == 0) badge = "An object";
    const S32 idx = act([this, id, enemy]() { mCharsWho = (enemy ? "e:" : "c:") + id; mScroll[SP_CHARS] = 0.f; });
    const bool hover = mHits.isHover(SH_E, idx);
    card(l, b, r, top, p, 6.f);
    if (hover) chamferEdge(l + 0.5f, b + 0.5f, r - 0.5f, top - 0.5f, 6.f, 2.f, p.mAccent);
    charsPortrait(l + (cw - CARD_PIC) * 0.5f, top - 8.f - CARD_PIC, CARD_PIC, sheet, enemy, p);
    F32 y = top - 8.f - CARD_PIC - 6.f;
    textL(bold, shown, l + cw * 0.5f, y, hover ? p.mAccent : p.mText, LLFontGL::HCENTER, LLFontGL::TOP, cw - 12.f);
    y -= lh + 2.f;
    textL(small, line, l + cw * 0.5f, y, p.mDim, LLFontGL::HCENTER, LLFontGL::TOP, cw - 12.f);
    y -= sh + 6.f;
    {
        const F32 bw = llmin(cw - 12.f, (F32)small->getWidthF32(badge) + 16.f), bl = l + (cw - bw) * 0.5f;
        const LLColor4 ink = dressed ? OK_INK : p.mDim;
        chamferEdge(bl, y - 20.f, bl + bw, y, 3.f, 1.f, ink);
        textL(small, badge, l + cw * 0.5f, y - 10.f, ink, LLFontGL::HCENTER, LLFontGL::VCENTER, bw - 8.f);
    }
    hit(l, b, r, top, SH_E, idx, "Open " + shown + ": " + (line.empty() ? words : line + ". " + words));
    if (mKeyboardNav && hasFocus() && mHits.isFocused(SH_E, idx)) focusRing(l, b, r, top, p);
    return ch;
}

F32 WolfAdventureStudioView::charsGallery(F32 l, F32 r, F32 top, const Palette& p)
{
    const LLSD& cast = std::as_const(mDraftPack)["cast"];
    const LLSD& enemies = std::as_const(mDraftPack)["enemies"];
    const S32 n = cast.isArray() ? (S32)cast.size() : 0;
    const S32 ne = enemies.isArray() ? (S32)enemies.size() : 0;
    F32 y = top;
    textL(display(28.f), "Your characters", l, y, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, r - l);
    y -= (F32)display(28.f)->getLineHeight() + 8.f;
    y = para(l, r, y, "These are the people in your story - click someone to check their name, what they say and how they look.", p.mText) - 10.f;
    {
        const S32 ia = act([this]() { mCharsError.clear(); addCastMember(); });
        btn(l, y - ROW_H, l + btnW("Add someone"), y, "Add someone", SH_E, ia, "A blank card for you to fill in", p, n < 100);
        // [IN-WORLD STUFF 2026-10-17] every look pad of the game off the land (look_pads_clear). [WORDING 2026-10-18] shown only while
        // the game has pads standing (get_draft `pads`, counted here as pads are put down and taken away)
        if (mPadCount > 0)
        {
            const S32 ic = act([this]() { lookPadsClear(); });
            const F32 cx = l + btnW("Add someone") + 8.f;
            btn(cx, y - ROW_H, cx + btnW("Take the pads away"), y, "Take the pads away", SH_E, ic, "Every look pad this game put on the land goes", p, !mPadBusy);
        }
        y -= ROW_H + 10.f;
    }
    y = lookPadsHead(l, r, y, p);   // [IN-WORLD STUFF 2026-10-17] "Anyone can help dress my characters" (small, when the brain has it)
    if (!mCharsError.empty()) y = para(l, r, y, mCharsError, DANGER_INK) - 6.f;
    if (n == 0) y = para(l, r, y, "No characters yet. Press Add someone, or go back to Story and press Build my game.", p.mDim, true) - 8.f;
    // the grid: 3 to 4 cards per row where the page is wide enough, 2 on a narrow page (Paul: "pictures for each character")
    const S32 cols = llclamp((S32)floorf((r - l + CARD_GAP) / (CARD_W + CARD_GAP)), 2, 4);
    auto grid = [&](const LLSD& list, S32 count, bool enemy)
    {
        F32 row_h = 0.f;
        for (S32 i = 0; i < count; ++i)
        {
            const S32 col = i % cols;
            if (col == 0 && i > 0) { y -= row_h + CARD_GAP; row_h = 0.f; }
            row_h = llmax(row_h, charsCard(l + (F32)col * (CARD_W + CARD_GAP), y, CARD_W, list[i], enemy, p));
        }
        if (count > 0) y -= row_h + CARD_GAP;
    };
    grid(cast, n, false);
    if (ne > 0)
    {
        // Source: adventure_pack_v1.schema.json "enemies" (the people and things the players fight), under their own head
        y = head(l, r, y - 4.f, "Enemies", p);
        grid(enemies, ne, true);
    }
    // [FIXED FOOT 2026-10-17] "Your story" and "Next: set up your places" are in the fixed bar under the page (drawFootBar)
    return y;
}

F32 WolfAdventureStudioView::charsDetail(F32 l, F32 r, F32 top, S32 at, bool enemy, const Palette& p)
{
    const boost::json::value& root = packSchema();
    const boost::json::value& cs = def(root, enemy ? "enemy" : "castSheet");
    const LLSD& list = std::as_const(mDraftPack)[enemy ? "enemies" : "cast"];
    const S32 n = list.isArray() ? (S32)list.size() : 0;
    const LLSD& c = list[at];
    const std::string cp = ptrIndex(enemy ? "/enemies" : "/cast", at);
    const std::string cid = c["id"].asString();
    const std::string nm = enemy ? cap(c["name"].asString(), 32) : cap(c["first"].asString() + " " + c["last"].asString(), 40);
    const std::string shown = nm.size() > 1 ? nm : std::string(enemy ? "New enemy" : "New character");
    F32 y = top;
    {
        // the way back, and Previous / Next through the same list (the characters, or the enemies)
        const S32 ib = act([this]() { mCharsWho.clear(); mScroll[SP_CHARS] = 0.f; });
        const std::string back = "\xE2\x97\x82 All characters";
        btn(l, y - ROW_H, l + btnW(back), y, back, SH_E, ib, "Back to the gallery of your characters", p);
        const std::string prev = "\xE2\x97\x82 Previous", next = enemy ? "Next enemy \xE2\x96\xB8" : "Next character \xE2\x96\xB8";
        const F32 nw = btnW(next), pw = btnW(prev);
        // the neighbours' ids, taken now (the draft may be read again before the press)
        const std::string prev_id = at > 0 ? list[at - 1]["id"].asString() : std::string();
        const std::string next_id = at + 1 < n ? list[at + 1]["id"].asString() : std::string();
        const std::string tag = enemy ? "e:" : "c:";
        const S32 ip = act([this, tag, prev_id]() { if (!prev_id.empty()) { mCharsWho = tag + prev_id; mScroll[SP_CHARS] = 0.f; } });
        const S32 in = act([this, tag, next_id]() { if (!next_id.empty()) { mCharsWho = tag + next_id; mScroll[SP_CHARS] = 0.f; } });
        btn(r - nw - 8.f - pw, y - ROW_H, r - nw - 8.f, y, prev, SH_E, ip, at > 0 ? "The one before this" : "This is the first", p, at > 0);
        btn(r - nw, y - ROW_H, r, y, next, SH_E, in, at + 1 < n ? "The one after this" : "This is the last", p, at + 1 < n);
        y -= ROW_H + 10.f;
    }
    textL(display(28.f), shown, l, y, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, r - l);
    y -= (F32)display(28.f)->getLineHeight() + 8.f;
    if (!mCharsError.empty()) y = para(l, r, y, mCharsError, DANGER_INK) - 6.f;
    y -= 10.f;
    // the left column: the big picture, Set picture... / No picture (a character's portrait, written as the cover is: chooseTexture
    // -> LLFloaterTexturePicker, docSet on /cast/N/portrait; a texture dragged from inventory onto it does the same, editDrop)
    const F32 pl = l + 12.f;
    charsPortrait(pl, y - DETAIL_PIC, DETAIL_PIC, c, enemy, p);
    F32 py = y - DETAIL_PIC - 8.f;
    if (!enemy)
    {
        const std::string pp = ptrJoin(cp, "portrait");
        mDrops.push_back({ LLRect((S32)pl, (S32)y, (S32)(pl + DETAIL_PIC), (S32)(y - DETAIL_PIC)), pp });
        const S32 ic = act([this, pp]() { chooseTexture(pp); });
        btn(pl, py - ROW_H, pl + DETAIL_PIC, py, "Set picture...", SH_E, ic, "Choose a picture from your inventory (or drag a texture onto the picture)", p);
        py -= ROW_H + 6.f;
        if (!c["portrait"].asString().empty())
        {
            const S32 ix = act([this, pp]() { docErase(0, pp); });
            btn(pl, py - ROW_H, pl + DETAIL_PIC, py, "No picture", SH_E, ix, "Take the picture away (their initials show instead)", p, true, BTN_DANGER);
            py -= ROW_H + 6.f;
        }
    }
    else
    {
        py = para(pl, pl + DETAIL_PIC, py, "An enemy that looks like one of your characters uses their picture.", p.mDim, true) - 6.f;
    }
    // the right column: the fields, in the old order
    const F32 fl = pl + DETAIL_PIC + 20.f, fr = r - 12.f;
    if (!enemy)
    {
        // [TIDY FORM 2026-10-17] Paul: "tidy up the form a bit" - labels above one column of boxes of the same width (mLabelsAbove),
        // in two groups, "Who they are" and "What they say", then one "Look and voice" row; Remove alone at the bottom right.
        // Source: adventure_pack_v1.schema.json castSheet: first, last (one word each), role (240), persona (2000), greeting (500),
        // neverReveal (up to 30 lines of 240) - the schema's caps apply through fValue, the same draft edit path as before
        const F32 gl = fl + 10.f, gr = fr - 10.f;
        auto group = [&](const std::string& title, const std::function<void()>& fields)
        {
            const F32 gt = y;
            y = head(gl, gr, y - 8.f, title, p) - 2.f;
            mLabelsAbove = true;
            fields();
            mLabelsAbove = false;
            y -= 4.f;
            chamferEdge(fl, y, fr, gt, 6.f, 1.f, fade(p.mDim, 0.6f));
            y -= 10.f;
        };
        group("Who they are", [&]()
        {
            y = fValue(gl, gr, y, ptrJoin(cp, "first"), "first", prop(cs, "first"), c["first"], false, 0, p, "First name");
            y = fValue(gl, gr, y, ptrJoin(cp, "last"), "last", prop(cs, "last"), c["last"], false, 0, p, "Last name");
            y = fValue(gl, gr, y, ptrJoin(cp, "role"), "role", prop(cs, "role"), c["role"], false, 0, p, "Who they are, in a few words");
            y = fValue(gl, gr, y, ptrJoin(cp, "persona"), "persona", prop(cs, "persona"), c["persona"], true, 0, p, "How they think and talk");
        });
        group("What they say", [&]()
        {
            y = fValue(gl, gr, y, ptrJoin(cp, "greeting"), "greeting", prop(cs, "greeting"), c["greeting"], true, 0, p, "First thing they say");
            y = fValue(gl, gr, y, ptrJoin(cp, "neverReveal"), "neverReveal", prop(cs, "neverReveal"), c["neverReveal"], true, 0, p, "What they never tell");
        });
        {
            // [IN-WORLD STUFF 2026-10-17] the look (the same calls: look_pad, save_look) and the voice (voiceRow: the Cast page's
            // picker and premium group; previewVoice), as one row
            const F32 gt = y;
            y = head(gl, gr, y - 8.f, "Look and voice", p) - 2.f;
            const std::string words = mLookCast == cid ? std::string("Saving what you are wearing...") : lookWords(c);
            const bool dressed = mLookCast != cid && words.rfind("Dressed", 0) == 0;
            y = para(gl, gr, y, words, dressed ? OK_INK : p.mDim, true) - 6.f;
            const S32 ip = act([this, cid, shown]() { lookPad(cid, shown); });
            const F32 pw = btnW("Put a look pad on the land") + 8.f;
            btn(gl, y - ROW_H, gl + pw, y, "Put a look pad on the land", SH_E, ip,
                "A round pad with their name appears at your feet; whoever clicks it wearing an outfit gives " + shown + " that look", p, mLookCast.empty() && !mPadBusy);
            const S32 iw = act([this, cid]() { saveLook(cid); });
            const F32 ww = btnW("Use what I'm wearing") + 8.f;
            btn(gl + pw + 6.f, y - ROW_H, gl + pw + 6.f + ww, y, "Use what I'm wearing", SH_E, iw,
                "Save your current outfit as this character's look (stand on the land this game runs on)", p, mLookCast.empty() && !mPadBusy);
            y -= ROW_H + 8.f;
            const bool has_voice = !c["voice"]["id"].asString().empty();
            const F32 hw = btnW("Hear them") + 8.f, vy = y;
            y = voiceRow(gl, gr - hw - 6.f, y, at, false, p);
            const S32 ih = act([this, at]() { previewVoice(at); });
            btn(gr - hw, vy - ROW_H, gr, vy, "Hear them", SH_E, ih, has_voice ? std::string("Hear them say their first line (only you)") : std::string("Choose a voice first"), p, has_voice && mBusy == 0);
            y -= 4.f;
            chamferEdge(fl, y, fr, gt, 6.f, 1.f, fade(p.mDim, 0.6f));
            y -= 10.f;
        }
    }
    else
    {
        // Source: adventure_pack_v1.schema.json $defs enemy: name (1 to 32, required; also the NPC's name tag), description (text500)
        y = fValue(fl, fr, y, ptrJoin(cp, "name"), "name", prop(cs, "name"), c["name"], false, 0, p, "Name");
        y = fValue(fl, fr, y, ptrJoin(cp, "description"), "description", prop(cs, "description"), c["description"], true, 0, p, "Who or what they are");
        const std::string words = enemyLookWords(c);
        const bool dressed = words.rfind("Dressed", 0) == 0;
        F32 cl = fl;
        fRow(fl, fr, y, "Look", cl);
        y = para(cl, fr, y - 6.f, words, dressed ? OK_INK : p.mDim, true) - 6.f;
        y = para(fl, fr, y, "How tough they are and what they look like is set in Advanced editing, on the Enemies step.", p.mDim, true) - 6.f;
    }
    y = llmin(y, py);
    y -= 6.f;
    if (!enemy)
    {
        // Remove, bottom right: asks first (SC_EDIT_DELETE); refused while the story uses them, with how many boxes name them
        const S32 ix = act([this, cp, cid, shown]()
        {
            // [SIMPLE STUDIO 2026-10-17] plain words: how many boxes name them (distinct "/acts/a/scenes/s/beats/b" pointers; a
            // use outside a box - a part's stage - counts as one), never the pointers themselves
            const std::vector<std::string> uses = castUsesList(cid);
            if (!uses.empty())
            {
                std::set<std::string> boxes;
                for (const std::string& u : uses)
                {
                    const size_t bp = u.find("/beats/");
                    if (bp == std::string::npos) { boxes.insert(u); continue; }
                    const size_t end = u.find('/', bp + 7);
                    boxes.insert(end == std::string::npos ? u : u.substr(0, end));
                }
                const S32 nb = (S32)boxes.size();
                // [WORDING 2026-10-18] the web Studio's words, then which boxes (by their titles) so the author can find them
                std::string which;
                S32 listed = 0;
                for (const std::string& bx : boxes)
                {
                    if (listed++ >= 6) { which += ", ..."; break; }
                    const LLSD& node = sdAt(mDraftPack, bx);
                    std::string t = node["title"].asString().empty() ? node["objective"].asString() : node["title"].asString();
                    if (t.empty()) t = bx.find("/beats/") == std::string::npos ? std::string("the start of a part") : std::string("a box with no words yet");
                    which += (which.empty() ? "" : ", ") + ("\"" + WolfGame::cleanText(t, 48) + "\"");
                }
                mCharsError = shown + llformat(" is in %d box%s of your story, so they can't be removed. Take them out of %s first: ", nb, nb == 1 ? "" : "es",
                                               nb == 1 ? "that box" : "those boxes") + which + ".";
                return;
            }
            mCharsError.clear();
            askDelete(0, cp, shown, true);
        });
        btn(r - 12.f - btnW("Remove"), y - ROW_H, r - 12.f, y, "Remove", SH_E, ix, "Take this character out of the game (asks first)", p, true, BTN_DANGER);
        y -= ROW_H + 6.f;
    }
    y -= 6.f;
    // [FIXED FOOT 2026-10-17] "Your story" and "Next: set up your places" are in the fixed bar under the page (drawFootBar)
    return y;
}

F32 WolfAdventureStudioView::pageChars(F32 l, F32 r, F32 top, const Palette& p)
{
    // [SIMPLE STUDIO 2026-10-17] page 2 "Your characters" (SIMPLE_STUDIO_SPEC.md); [CHARACTER GALLERY 2026-10-17] a gallery of
    // picture cards (charsGallery), and a card opens that one's page (charsDetail). Ids, home, manner, idle lines, barks, cards,
    // may do, fights and never says stay on the Cast page (Advanced editing).
    if (mOpenId.empty())
    {
        F32 y = para(l, r, top, "Make your game first: go to Story and press Build my game.", p.mDim) - 8.f;
        const S32 ib = act([this]() { press(SH_PAGE, (S32)SP_HELPER); });
        btn(l, y - 30.f, l + btnW("\xE2\x97\x82 Story"), y, "\xE2\x97\x82 Story", SH_E, ib, "Back to Story", p, true, BTN_PRIMARY);
        return y - 38.f;
    }
    if (!openIsOwner())
    {
        F32 y = para(l, r, top, "Only the game's owner sets up its characters. As a host or teacher, use Publish to see it and Run to run it live.", p.mDim) - 8.f;
        const S32 ip = act([this]() { press(SH_PAGE, (S32)SP_PUBLISH); });
        btn(l, y - 30.f, l + btnW("Publish"), y, "Publish", SH_E, ip, "See the game and where it runs", p);
        return y - 38.f;
    }
    if (mDraftJson.empty()) return waiting(l, r, top, "Opening your game...", p);
    // [GAME CARD 2026-10-18] GAME_CARD_SPINE_SPEC.md section 5: the game card at the top of page 2, always
    const F32 y = gameCard(l, r, conflictBanner(l, r, top, p), p);
    if (mCharsWho.size() > 2)
    {
        // one character's (or enemy's) page; gone from the draft (removed, or the helper rewrote the cast) -> the gallery
        const bool enemy = mCharsWho[0] == 'e';
        const std::string id = mCharsWho.substr(2);
        const LLSD& list = std::as_const(mDraftPack)[enemy ? "enemies" : "cast"];
        for (S32 i = 0; list.isArray() && i < (S32)list.size(); ++i)
            if (list[i]["id"].asString() == id) return charsDetail(l, r, y, i, enemy, p);
        mCharsWho.clear();
    }
    return charsGallery(l, r, y, p);
}

bool WolfAdventureStudioView::placesPageShown() const
{
    const LLFloater* f = getParentByType<LLFloater>();
    // [WOLF ADVENTURE AUDIT 2026-10-17] the wizard's Places step draws them too
    const bool places = mPage == SP_PLACES || (mPage == SP_WIZARD && mWizStep == W_PLACES);
    return places && f && f->getVisible() && !f->isMinimized() && !mOpenId.empty();
}

F32 WolfAdventureStudioView::pagePlaces(F32 l, F32 r, F32 top, const Palette& p)
{
    if (!openIsOwner()) return waiting(l, r, top, "Only the adventure's owner edits its places.", p);
    if (mDraftJson.empty()) return waiting(l, r, top, "Opening the draft...", p);
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    // the tool draws every place on this region while this page shows (§2.4 "Overlay", through its idle render
    // hook); it becomes the current tool only while a shape is being placed (startPlace)
    WolfToolAdventurePlace* tool = WolfToolAdventurePlace::getInstance();
    if (!tool->attached()) tool->attach(getDerivedHandle<WolfAdventureStudioView>());
    LLViewerRegion* rgn = gAgent.getRegion();
    const std::string here = rgn ? rgn->getRegionID().asString() : std::string();
    F32 y = head(l, r, conflictBanner(l, r, top, p), "Places", p);
    y = para(l, r, y, "Spots, areas and paths are drawn in the world while this page is open. Place them with the buttons; places on other regions offer Teleport there.", p.mDim, true) - 4.f;
    if (tool->mode() != WolfToolAdventurePlace::M_NONE)
    {
        y = para(l, r, y, tool->hint(), WARN_INK, true);
        const S32 ic = act([tool]() { tool->cancel(); });
        btn(l, y - ROW_H, l + 140.f, y, "Stop placing", SH_E, ic, "Stop the place tool (Escape in the world does the same)", p);
        y -= ROW_H + 8.f;
    }
    static const char* TABS[4] = { "Spots", "Areas", "Paths", "Chests" };
    static const char* KEYS[4] = { "spots", "areas", "paths", "chests" };
    F32 x = l;
    for (S32 i = 0; i < 4; ++i)
    {
        const S32 idx = act([this, i]() { mPlaceTab = i; });
        const S32 cnt = std::as_const(mDraftPack)["places"][KEYS[i]].isArray() ? (S32)std::as_const(mDraftPack)["places"][KEYS[i]].size() : 0;
        const std::string tab = llformat("%s (%d)", TABS[i], cnt);
        const F32 bw = btnW(tab, 100.f);
        if (x > l && x + bw > r) { x = l; y -= ROW_H + 4.f; }
        btn(x, y - ROW_H, x + bw, y, tab, SH_E, idx, std::string("Show the ") + TABS[i], p, true, mPlaceTab == i ? BTN_ACTIVE : 0);
        x += bw + 4.f;
    }
    y -= ROW_H + 8.f;
    const std::string key = KEYS[mPlaceTab];
    const std::string arr = "/places/" + key;
    const LLSD& list = std::as_const(mDraftPack)["places"][key];
    const boost::json::value& ps = prop(def(packSchema(), "places"), key);
    const boost::json::value& isch = ps.is_object() && ps.as_object().if_contains("items") ? ps.as_object().at("items") : nullV();
    const S32 n = list.isArray() ? (S32)list.size() : 0;
    for (S32 i = 0; i < n; ++i)
    {
        const LLSD& it = list[i];
        const std::string ip = ptrIndex(arr, i);
        std::string region_name;
        for (const LLSD& rg : llsd::inArray(std::as_const(mDraftPack)["adventure"]["regions"])) if (rg["region"].asString() == it["region"].asString()) region_name = rg["name"].asString();
        const bool on_here = it["region"].asString() == here;
        bool unplaced = false;
        if (it["pos"].isArray()) unplaced = it["pos"][0].asReal() < 0.0 || it["pos"][1].asReal() < 0.0;
        if (it["shape"]["circle"].isMap()) unplaced = it["shape"]["circle"]["centre"][0].asReal() < 0.0;
        if (it["shape"]["polygon"].isArray()) unplaced = it["shape"]["polygon"][0][0].asReal() < 0.0;
        if (it["points"].isArray()) unplaced = it["points"][0][0].asReal() < 0.0;
        std::string title = cap(it["label"].asString().empty() ? it["id"].asString() : it["label"].asString(), 40);
        title += " - " + (region_name.empty() ? std::string("(region not in the adventure)") : cap(region_name, 30)) + (on_here ? " (here)" : "");
        if (unplaced) title += " - UNPLACED";
        y = cardHead(l, r, y, ip, title, i, n, arr, true, p);
        if (!mOpen.count(ip)) continue;
        y = fObject(l + 14.f, r, y, ip, isch, it, 0, p);
        // the in-world tools (§2.4)
        F32 bx = l + 14.f;
        const std::string pid = it["id"].asString();
        const bool can_here = on_here || it["region"].asString().empty();
        auto place_btn = [&](const std::string& lab, WolfToolAdventurePlace::EMode mode)
        {
            const std::string what = "\"" + cap(it["label"].asString().empty() ? pid : it["label"].asString(), 32) + "\"";
            const S32 idx = act([this, key, pid, mode, what]() { startPlace(key, pid, mode, what); });
            btn(bx, y - ROW_H, bx + 150.f, y, lab, SH_E, idx, can_here ? lab + " on this region with the mouse (STUDIO §2.4)" : "It is on another region: Teleport there first",
                p, can_here && !pid.empty(), unplaced ? BTN_PRIMARY : 0);
            bx += 154.f;
        };
        if (key == "spots")
        {
            place_btn(unplaced ? "Place now" : "Place in world", WolfToolAdventurePlace::M_SPOT);
            // where the owner stands, facing the way they face (one click, §6 3.3.7)
            const S32 im = act([this, pid]() { placeUseMyPosition(pid); });
            btn(bx, y - ROW_H, bx + 150.f, y, "Use my position", SH_E, im, can_here ? "Put the spot where you stand, facing the way you face" : "It is on another region: Teleport there first",
                p, can_here && !pid.empty());
            bx += 154.f;
        }
        if (key == "areas")
        {
            // [WOLF ADVENTURE AUDIT 2026-10-17] one click, as for spots (the brain's plain problems say "Use my position")
            const S32 im = act([this, pid]() { placeAreaHere(pid); });
            btn(bx, y - ROW_H, bx + 150.f, y, "Use my position", SH_E, im, can_here ? "A circle round where you stand (its radius is kept, else 5 m)" : "It is on another region: Teleport there first",
                p, can_here && !pid.empty(), unplaced ? BTN_PRIMARY : 0);
            bx += 154.f;
            place_btn("Draw circle", WolfToolAdventurePlace::M_CIRCLE);
            place_btn("Draw polygon", WolfToolAdventurePlace::M_POLYGON);
        }
        if (key == "paths") place_btn("Draw path", WolfToolAdventurePlace::M_PATH);
        if (key == "chests")
        {
            // §2.4 Chest: "Shows how to bind: drop the 'Wolf Adventure chest' script into an object and type the chest id"
            // [WOLF ADVENTURE AUDIT 2026-10-17] the binding call is the region's wolfAdventureChest(adventure, chestId, on)
            // (REGION_HANDS_V1.md:397: "Bind THIS prim as the pack's chest chestId on this region"; the prim's owner must be
            // the adventure's owner). The script is put on the clipboard with this adventure and this chest filled in.
            y = para(l + 14.f, r, y, "To make a real chest: 1. Rez a box on " + (region_name.empty() ? std::string("its region") : region_name) +
                     " and put the items to give inside it (they must be copyable). 2. Press Copy the chest script. 3. In the box, make a New Script, "
                     "select all its text, paste over it and save. The box says when it is bound.", p.mDim, true) - 4.f;
            // the title sits in a // comment: no line break or quote may end it early
            std::string chestTitle = cap(mOpenTitle, 64);
            for (char& ch : chestTitle) if (ch == '\n' || ch == '\r' || (unsigned char)ch < 0x20) ch = ' ';
            const std::string script = llformat(
                "// Wolf Adventure chest: binds this box as the chest \"%s\" of the adventure \"%s\".\n"
                "default\n{\n    state_entry()\n    {\n"
                "        integer ok = wolfAdventureChest(\"%s\", \"%s\", TRUE);\n"
                "        if (ok) llOwnerSay(\"Bound as the chest %s.\");\n"
                "        else llOwnerSay(\"Not bound: the box must be yours (the adventure's owner), hold only copyable items, and be on a region the adventure runs on.\");\n"
                "    }\n}\n",
                it["id"].asString().c_str(), chestTitle.c_str(), mOpenId.c_str(), it["id"].asString().c_str(), it["id"].asString().c_str());
            const S32 ic = act([this, script]()
            {
                LLClipboard::instance().copyToClipboard(utf8str_to_wstring(script), 0, (S32)utf8str_to_wstring(script).size());
                setStatus("The chest script is on the clipboard: paste it into a New Script in the box.", false);
            });
            btn(l + 14.f, y - ROW_H, l + 14.f + btnW("Copy the chest script"), y, "Copy the chest script", SH_E, ic,
                "Copy a ready-made script that binds a box as this chest", p, !mOpenId.empty() && !it["id"].asString().empty());
            y -= ROW_H + 10.f;
        }
        else if (!on_here && !region_name.empty())
        {
            F32 tx = 128.f, ty = 128.f, tz = 0.f;   // an unplaced place: the region's middle
            if (unplaced) {}
            else if (it["pos"].isArray()) { tx = (F32)it["pos"][0].asReal(); ty = (F32)it["pos"][1].asReal(); tz = (F32)it["pos"][2].asReal(); }
            else if (it["shape"]["circle"].isMap()) { tx = (F32)it["shape"]["circle"]["centre"][0].asReal(); ty = (F32)it["shape"]["circle"]["centre"][1].asReal(); }
            else if (it["shape"]["polygon"].isArray()) { tx = (F32)it["shape"]["polygon"][0][0].asReal(); ty = (F32)it["shape"]["polygon"][0][1].asReal(); }
            else if (it["points"].isArray()) { tx = (F32)it["points"][0][0].asReal(); ty = (F32)it["points"][0][1].asReal(); }
            const std::string rn = region_name;
            const S32 idx = act([this, rn, tx, ty, tz]()
            {
                mConfirmKeyA = rn;
                mConfirmKeyB = llformat("%.1f,%.1f,%.1f", tx, ty, tz);
                ask(SC_PLACE_TELEPORT, 0);
            });
            btn(bx, y - ROW_H, bx + 150.f, y, "Teleport there...", SH_E, idx, "Teleport to " + rn + " to place it (asks first)", p);
        }
        if (key != "chests") y -= ROW_H + 10.f;
    }
    {
        const std::string region = here.empty() ? std::as_const(mDraftPack)["adventure"]["regions"][0]["region"].asString() : here;
        const boost::json::value* ip = &isch;
        const std::string next = ptrIndex(arr, n);
        const S32 idx = act([this, arr, ip, key, region, next]() { docAppend(0, arr, makeDefault(packSchema(), *ip, key, mDraftJson, region)); mOpen.insert(next); });
        btn(l, y - ROW_H, l + 160.f, y, std::string("Add ") + labelFor(idBase(key)), SH_E, idx, "Add one on the region you are on", p);
        y -= ROW_H + 8.f;
        (void)small;
    }
    return y;
}

const LLSD& WolfAdventureStudioView::placeOf(const std::string& kind, const std::string& id) const
{
    static const LLSD none;
    for (const LLSD& x : llsd::inArray(std::as_const(mDraftPack)["places"][kind])) if (x["id"].asString() == id) return x;
    return none;
}

std::string WolfAdventureStudioView::placePtr(const std::string& kind, const std::string& id) const
{
    const LLSD& list = std::as_const(mDraftPack)["places"][kind];
    for (size_t i = 0; list.isArray() && i < (size_t)list.size(); ++i)
        if (list[(LLSD::Integer)i]["id"].asString() == id) return ptrIndex("/places/" + ptrEscape(kind), (S32)i);
    return std::string();
}

void WolfAdventureStudioView::placeSay(const std::string& what)
{
    if (what.empty()) return;
    // the tool's refusals read as errors (an icon and the words), its progress as plain status
    const bool err = what.find("Click on") == 0 || what.find("needs") != std::string::npos || what.find("no longer") != std::string::npos
                  || what.find("left the region") != std::string::npos || what.find("most points") != std::string::npos || what.find("edge of the region") != std::string::npos
                  || what.find("first, then") != std::string::npos;
    setStatus(what, err);
}

void WolfAdventureStudioView::startPlace(const std::string& kind, const std::string& id, S32 mode, const std::string& what)
{
    commitEdit();
    mField = F_NONE;
    // STUDIO §2.4: a place is drawn on its own region; elsewhere the page offers Teleport there
    LLViewerRegion* rgn = gAgent.getRegion();
    const LLSD& pl = placeOf(kind, id);
    if (!pl.isMap()) { setStatus("That place is no longer in the draft.", true); return; }
    const std::string reg = pl["region"].asString();
    if (!rgn || (!reg.empty() && reg != rgn->getRegionID().asString()))
    {
        setStatus("This place is on another region: use Teleport there, then place it.", true);
        return;
    }
    WolfToolAdventurePlace* tool = WolfToolAdventurePlace::getInstance();
    tool->attach(getDerivedHandle<WolfAdventureStudioView>());
    tool->begin(kind, id, (WolfToolAdventurePlace::EMode)mode, what);
    if (tool->mode() != WolfToolAdventurePlace::M_NONE) setStatus(tool->hint(), false);
}

void WolfAdventureStudioView::placeUseMyPosition(const std::string& id)
{
    LLViewerRegion* rgn = gAgent.getRegion();
    const LLSD& pl = placeOf("spots", id);
    if (!pl.isMap()) { setStatus("That spot is no longer in the draft.", true); return; }
    if (!rgn || (!pl["region"].asString().empty() && pl["region"].asString() != rgn->getRegionID().asString()))
    {
        setStatus("This spot is on another region: use Teleport there first.", true);
        return;
    }
    // llagent.h getPositionAgent / getAtAxis; facing 0 = east, 90 = north (adventure_pack_v1.schema.json spots facing)
    const LLVector3 p = rgn->getPosRegionFromAgent(gAgent.getPositionAgent());
    const LLVector3 at = gAgent.getAtAxis();
    if (!std::isfinite(p.mV[VX]) || !std::isfinite(p.mV[VY]) || !std::isfinite(p.mV[VZ])) return;
    F32 facing = std::round(atan2f(at.mV[VY], at.mV[VX]) * RAD_TO_DEG);
    if (facing < 0.f) facing += 360.f;
    const F32 w = rgn->getWidth();
    auto d2 = [](F32 v) { return std::round((double)v * 100.0) / 100.0; };
    boost::json::object o;
    o["pos"] = boost::json::array{ d2(llclamp(p.mV[VX], 0.f, w - 0.01f)), d2(llclamp(p.mV[VY], 0.f, w - 0.01f)), d2(p.mV[VZ]) };
    o["facing"] = d2(facing);
    o["region"] = rgn->getRegionID().asString();
    placed("spots", id, o, llformat("The spot is where you stand, facing %.0f degrees.", facing));
}

void WolfAdventureStudioView::placed(const std::string& kind, const std::string& id, const boost::json::value& v, const std::string& what)
{
    if (!v.is_object()) return;
    // the place found again by its id (a list may have been reordered since the tool began)
    const std::string ptr = placePtr(kind, id);
    if (ptr.empty())
    {
        stopPlacing();
        setStatus("That place is no longer in the draft - nothing was written.", true);
        return;
    }
    // the place's own members are replaced by what the tool drew (pos/facing, shape, points, and its region)
    const boost::json::object& o = v.as_object();
    if (!editDoc([&ptr, &o](boost::json::value& root)
    {
        boost::json::value* at = jAt(root, ptr, false);
        if (!at || !at->is_object()) return false;
        // whole members: a drawn shape replaces the shape there was (circle or polygon, schema areas shape oneOf)
        for (const auto& kv : o) at->as_object()[kv.key()] = kv.value();
        return true;
    }))
    {
        if (!mDraftJson.empty()) setStatus(STALE, true);
        return;
    }
    // the region must be one of the adventure's (else the brain refuses the place): added when missing
    if (o.contains("region") && o.at("region").is_string())
    {
        const std::string rid(o.at("region").as_string().c_str());
        bool listed = false;
        for (const LLSD& rg : llsd::inArray(std::as_const(mDraftPack)["adventure"]["regions"])) listed = listed || rg["region"].asString() == rid;
        LLViewerRegion* rgn = gAgent.getRegion();
        if (!listed && rgn && rgn->getRegionID().asString() == rid)
            docAppend(0, "/adventure/regions", boost::json::object{ { "region", rid }, { "name", cap(rgn->getName(), 64) } });
    }
    if (!what.empty()) setStatus(what, false);
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// rules: the preset, the merged copy, and the overrides as an RFC 7396 merge patch
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

namespace
{
    const char* PRESET_IDS[5] = { "wild-west", "mystery", "education", "experience", "5e-srd-5.2" };
    /** A shipped preset file (app_settings/wolfadventure/rules/<id>.json, copied from the spec), read once. */
    const boost::json::value& presetFile(const std::string& id)
    {
        static std::map<std::string, boost::json::value> cache;
        auto it = cache.find(id);
        if (it != cache.end()) return it->second;
        bool known = false;
        for (const char* k : PRESET_IDS) known = known || id == k;
        boost::json::value v = known ? loadJson(gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "wolfadventure", "rules", id + ".json")) : boost::json::value();
        return cache.emplace(id, std::move(v)).first->second;
    }
    /** The same, as LLSD for drawing (made once). */
    const LLSD& presetSd(const std::string& id)
    {
        static std::map<std::string, LLSD> cache;
        auto it = cache.find(id);
        if (it != cache.end()) return it->second;
        const boost::json::value& pf = presetFile(id);
        return cache.emplace(id, pf.is_object() ? LlsdFromJson(pf) : LLSD()).first->second;
    }
    /** Equal as JSON values, a number being a number whatever its storage (LLSD turns 1 into 1.0 and back). */
    bool jEq(const boost::json::value& a, const boost::json::value& b)
    {
        if (a.is_number() && b.is_number())
        {
            boost::system::error_code e1, e2;
            const double x = a.to_number<double>(e1), y = b.to_number<double>(e2);
            return !e1 && !e2 && x == y;
        }
        if (a.kind() != b.kind()) return false;
        if (a.is_object())
        {
            const boost::json::object& oa = a.as_object();
            const boost::json::object& ob = b.as_object();
            if (oa.size() != ob.size()) return false;
            for (const auto& kv : oa)
            {
                const boost::json::value* o = ob.if_contains(kv.key());
                if (!o || !jEq(kv.value(), *o)) return false;
            }
            return true;
        }
        if (a.is_array())
        {
            const boost::json::array& aa = a.as_array();
            const boost::json::array& ab = b.as_array();
            if (aa.size() != ab.size()) return false;
            for (size_t i = 0; i < aa.size(); ++i) if (!jEq(aa[i], ab[i])) return false;
            return true;
        }
        return a == b;
    }
    /** RFC 7396 §2 MergePatch(Target, Patch). */
    boost::json::value mergePatch(const boost::json::value& target, const boost::json::value& patch)
    {
        if (!patch.is_object()) return patch;
        boost::json::object out = target.is_object() ? target.as_object() : boost::json::object();
        for (const auto& kv : patch.as_object())
        {
            if (kv.value().is_null()) out.erase(kv.key());
            else
            {
                const boost::json::value* t = out.if_contains(kv.key());
                out[kv.key()] = mergePatch(t ? *t : boost::json::value(), kv.value());
            }
        }
        return out;
    }
    /** The smallest merge patch that turns `from` into `to` (both objects): arrays and scalars whole, members
     *  gone become null (RFC 7396 §2: "null ... signal[s] the removal"). */
    boost::json::object diffPatch(const boost::json::object& from, const boost::json::object& to)
    {
        boost::json::object patch;
        for (const auto& kv : to)
        {
            const boost::json::value* f = from.if_contains(kv.key());
            if (!f) { patch[kv.key()] = kv.value(); continue; }
            if (jEq(*f, kv.value())) continue;
            if (f->is_object() && kv.value().is_object())
            {
                boost::json::object sub = diffPatch(f->as_object(), kv.value().as_object());
                if (!sub.empty()) patch[kv.key()] = std::move(sub);
            }
            else patch[kv.key()] = kv.value();
        }
        for (const auto& kv : from) if (!to.contains(kv.key())) patch[kv.key()] = nullptr;
        return patch;
    }
}

void WolfAdventureStudioView::loadPreset()
{
    // RULES_PRESETS_V1.md §3: the effective rules = the preset with rules.overrides merged (RFC 7396)
    // once per draft change (mDraftGen), not every frame
    if (mRulesGen == mDraftGen) return;
    mRulesGen = mDraftGen;
    const std::string id = std::as_const(mDraftPack)["rules"]["preset"].asString();
    mPresetId = id;
    const boost::json::value& pf = presetFile(id);
    mPreset = presetSd(id);
    // the overrides from the parsed draft itself (LLSD loses the nulls a merge patch deletes with)
    boost::json::value patch;
    if (mDraftDoc.is_object())
        if (const boost::json::value* o = jAt(mDraftDoc, "/rules/overrides", false)) patch = *o;
    mRulesMerged = pf.is_object() ? LlsdFromJson(mergePatch(pf, patch)) : LLSD();
}

bool WolfAdventureStudioView::rulesOverridden(const std::string& ptr) const
{
    return sdAt(std::as_const(mDraftPack)["rules"]["overrides"], ptr).isDefined();
}

void WolfAdventureStudioView::rulesSet(const std::string& ptr, const boost::json::value* v)
{
    // RULES_PRESETS_V1.md §3 "Creator overrides ... may not touch id, format, formatVersion, version or attribution"
    const std::vector<std::string> t = ptrTokens(ptr);
    static const char* LOCKED[5] = { "id", "format", "formatVersion", "version", "attribution" };
    for (const char* k : LOCKED)
        if (!t.empty() && t[0] == k) { setStatus("The preset's " + std::string(k) + " cannot be changed.", true); return; }
    const boost::json::value& pf = presetFile(mPresetId);
    if (!pf.is_object()) { setStatus("This pack's rules preset is not one this viewer knows - choose a preset first.", true); return; }
    boost::json::value merged = toJson(mRulesMerged);
    if (!merged.is_object()) merged = pf;
    if (v)
    {
        boost::json::value* at = jAt(merged, ptr, true);
        if (!at) { setStatus(STALE, true); return; }
        *at = *v;
    }
    else if (!jErase(merged, ptr)) { setStatus(STALE, true); return; }
    const boost::json::object patch = diffPatch(pf.as_object(), merged.as_object());
    editDoc([&patch](boost::json::value& root)
    {
        boost::json::value* rules = jAt(root, "/rules", true);
        if (!rules || !rules->is_object()) return false;
        if (patch.empty()) rules->as_object().erase("overrides");
        else rules->as_object()["overrides"] = patch;
        return true;
    });
    mRulesGen = 0;
    loadPreset();
}

namespace
{
    /** Every string value under the given member names anywhere in the pack (the stats and checks it uses). */
    void collectUses(const LLSD& v, const char* key, std::set<std::string>& out)
    {
        if (v.isMap())
        {
            for (const auto& kv : llsd::inMap(v))
            {
                if (kv.first == key && kv.second.isString() && !kv.second.asString().empty()) out.insert(kv.second.asString());
                else collectUses(kv.second, key, out);
            }
        }
        else if (v.isArray()) for (const LLSD& x : llsd::inArray(v)) collectUses(x, key, out);
    }
}

F32 WolfAdventureStudioView::pageRules(F32 l, F32 r, F32 top, const Palette& p)
{
    if (!openIsOwner()) return waiting(l, r, top, "Only the adventure's owner edits its rules.", p);
    if (mDraftJson.empty()) return waiting(l, r, top, "Opening the draft...", p);
    loadPreset();
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    F32 y = conflictBanner(l, r, top, p);
    y = head(l, r, y, "Rules preset", p);
    // §2.6 "Preset: five cards"
    const F32 gap = 8.f, cw = (r - l - 2.f * gap) / 3.f, ch = 128.f;
    for (S32 i = 0; i < 5; ++i)
    {
        const S32 col = i % 3, row = i / 3;
        const F32 cl = l + col * (cw + gap), ct = y - row * (ch + gap), cb = ct - ch;
        const std::string id = PRESET_IDS[i];
        const LLSD& ps = presetSd(id);
        const bool current = id == mPresetId;
        card(cl, cb, cl + cw, ct, p, 6.f);
        if (current) chamferEdge(cl, cb, cl + cw, ct, 6.f, 2.f, p.mAccent);
        F32 yy = ct - 8.f;
        textL(bold, cap(ps["title"].asString(), 64), cl + 8.f, yy, current ? p.mAccent : p.mText, LLFontGL::LEFT, LLFontGL::TOP, cw - 16.f);
        yy -= (F32)bold->getLineHeight() + 2.f;
        if (id == "5e-srd-5.2")
        {
            // RULES_PRESETS_V1.md §6.2: the "5E compatible" label and the SRD attribution, always shown
            textL(small, cap(ps["label"].asString(), 64), cl + 8.f, yy, WARN_INK, LLFontGL::LEFT, LLFontGL::TOP, cw - 16.f);
            yy -= (F32)small->getLineHeight();
        }
        for (const std::string& line : wrap(small, cap(ps["summary"].asString(), 500), cw - 16.f, id == "5e-srd-5.2" ? 2 : 4))
        {
            textL(small, line, cl + 8.f, yy, p.mDim);
            yy -= (F32)small->getLineHeight();
        }
        if (ps["status"].asString() == "skeleton")
        {
            textL(small, "Skeleton: the data tables come later", cl + 8.f, yy, WARN_INK, LLFontGL::LEFT, LLFontGL::TOP, cw - 16.f);
            yy -= (F32)small->getLineHeight();
        }
        const S32 idx = act([this, id, ps]()
        {
            // §2.6 "Changing the preset asks first, and shows which stats and checks the pack uses that the new preset lacks"
            std::set<std::string> stats, checks;
            collectUses(mDraftPack, "stat", stats);
            collectUses(mDraftPack, "check", checks);
            std::string missing;
            for (const std::string& s : stats) if (!ps["stats"].has(s)) missing += (missing.empty() ? "" : ", ") + std::string("stat ") + s;
            for (const std::string& c : checks) if (!ps["checks"].has(c)) missing += (missing.empty() ? "" : ", ") + std::string("check ") + c;
            mPresetPick = id;
            mConfirmKeyA = cap(ps["title"].asString(), 64);
            mConfirmKeyB = missing;
            ask(SC_PRESET, 0);
        });
        btn(cl + 8.f, cb + 8.f, cl + cw - 8.f, cb + 8.f + ROW_H, current ? "In use" : "Use this preset", SH_E, idx,
            current ? "This is the pack's preset" : "Change the pack's rules to " + ps["title"].asString() + " (asks first)", p, !current, current ? BTN_ACTIVE : 0);
    }
    y -= 2.f * (ch + gap) + 4.f;
    if (mPresetId == "5e-srd-5.2")
    {
        for (const LLSD& a : llsd::inArray(std::as_const(mPreset)["attribution"])) y = para(l, r, y, cap(a["text"].asString(), 1000), p.mDim, true) - 2.f;
        y -= 6.f;
    }
    // §2.6 "The game link"
    y = head(l, r, y, "Rewards and the wolfGame game", p);
    y = fText(l, r, y, "Game (blank = the region's own)", "/rules/game", std::as_const(mDraftPack)["rules"]["game"], 0, 36, 's', true,
              "The wolfGame game whose stats, items and money rewards use; blank = each region's own game (adventure_pack_v1 rules.game)", p);
    {
        if (mGameLinksFor != mOpenId && !mGameLinksLoading)
        {
            mGameLinksLoading = true;
            LLSD body;
            body["adventure"] = mOpenId;
            const std::string id = mOpenId;
            LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
            // BRAIN_API_V1.md §5.4 game_links {adventure} -> {links:[{game, adventure, title, by, at}]}
            WolfAdventure::api("game_links", body, [handle, id](bool ok, const LLSD& reply, const std::string& error)
            {
                if (handle.isDead()) return;
                WolfAdventureStudioView* self = handle.get();
                self->mGameLinksLoading = false;
                self->mGameLinksFor = id;
                self->mGameLinks = ok && reply["links"].isArray() ? reply["links"] : LLSD::emptyArray();
                self->mGameLinksError = ok ? std::string() : error;
            });
        }
        const std::string game = std::as_const(mDraftPack)["rules"]["game"].asString();
        bool linked = false;
        for (const LLSD& lk : llsd::inArray(mGameLinks)) linked = linked || (!game.empty() && lk["game"].asString() == game);
        // BRAIN_API_V1.md §8.3: value moves when the adventure's owner owns the game, or the game's owner linked it
        std::string status;
        if (!mGameLinksError.empty()) status = "The link status could not be read: " + mGameLinksError;
        else if (game.empty()) status = "Each region's own game. Items and money move only on games you own or whose owner linked this adventure; otherwise stats go to the adventure sheet and items and money are skipped (and logged).";
        else if (linked) status = "Linked: this game's owner lets the adventure move its value.";
        else status = "Not linked. If you own this game its value moves anyway; if not, ask its owner to link this adventure. Until then stats go to the adventure sheet and items and money are skipped.";
        y = para(l + LABEL_W, r, y, status, mGameLinksError.empty() ? p.mDim : DANGER_INK, true) - 4.f;
        if (!game.empty())
        {
            // §2.6 "the game's owner can link it from here when that is the same person" (link_game refuses anyone else)
            const S32 idx = act([this, game, linked]()
            {
                LLSD body;
                body["game"] = game;
                body["adventure"] = mOpenId;
                body["on"] = !linked;
                call("link_game", body, [linked](WolfAdventureStudioView* self, const LLSD&)
                {
                    self->mGameLinksFor.clear();
                    self->setStatus(linked ? "Unlinked: the adventure no longer moves this game's value." : "Linked: the adventure may move this game's value.", false);
                }, linked ? "Unlinking..." : "Linking...");
            });
            btn(l + LABEL_W, y - ROW_H, l + LABEL_W + 240.f, y, linked ? "Unlink my game" : "Link my game to this adventure", SH_E, idx,
                "Only the game's owner may do this; anyone else is refused with the reason", p);
            y -= ROW_H + 8.f;
        }
    }
    // §2.6 "The editor: tables for stats, checks, difficulties, conditions, combat, progression and creation"
    static const char* TABS[9] = { "Stats", "Checks", "Difficulties", "Conditions", "Combat", "Progression", "Creation", "Dice", "Flags and reputation" };
    static const char* KEYS[9] = { "stats", "checks", "difficulties", "conditions", "combat", "progression", "creation", "dice", "" };
    y = head(l, r, y, "The rules", p);
    {
        F32 x = l;
        for (S32 i = 0; i < 9; ++i)
        {
            const F32 bw = btnW(TABS[i]);
            if (x + bw > r) { x = l; y -= ROW_H + 4.f; }
            const S32 idx = act([this, i]() { mRulesTab = i; });
            btn(x, y - ROW_H, x + bw, y, TABS[i], SH_E, idx, std::string("Show ") + TABS[i], p, true, mRulesTab == i ? BTN_ACTIVE : 0);
            x += bw + 4.f;
        }
        y -= ROW_H + 10.f;
    }
    if (mRulesTab == 8)
    {
        // the pack's own state: flags, factions, reputation and honour (ADVENTURE_PACK_V1.md §6)
        y = para(l, r, y, "Flags remember what has happened; factions and honour are the reputations the story keeps.", p.mDim, true) - 4.f;
        return fObject(l, r, y, "/state", def(packSchema(), "state"), std::as_const(mDraftPack)["state"], 0, p);
    }
    if (!mRulesMerged.isMap()) return para(l, r, y, "This preset's file is missing from the viewer - reinstall WolfViewer.", DANGER_INK);
    y = para(l, r, y, "These start as the preset has them. A changed value shows a dot and \"changed\"; Reset to preset puts it back. Changes are kept as the pack's rule overrides.", p.mDim, true) - 4.f;
    const std::string key = KEYS[mRulesTab];
    const boost::json::value& rs = rulesSchema();
    const boost::json::value& ks = prop(rs, key);
    const bool map = ks.is_object() && ks.as_object().if_contains("additionalProperties") && ks.as_object().at("additionalProperties").is_object();
    if (map)
    {
        const boost::json::value& item = ks.as_object().at("additionalProperties");
        return fMap(l + 10.f, r, y, "/" + key, key, item, std::as_const(mRulesMerged)[key], 1, p, TABS[mRulesTab]);
    }
    if (rulesOverridden("/" + key))
    {
        const std::string kp = "/" + key;
        const S32 idx = act([this, kp]()
        {
            const LLSD& pv = sdAt(mPreset, kp);
            if (pv.isDefined()) { const boost::json::value v = toJson(pv); rulesSet(kp, &v); }
            else rulesSet(kp, nullptr);
        });
        disc(l + 14.f, y - ROW_H * 0.5f, 3.5f, WARN_INK);
        textL(small, "changed", l + 22.f, y - ROW_H * 0.5f, WARN_INK, LLFontGL::LEFT, LLFontGL::VCENTER);
        btn(l + 90.f, y - ROW_H, l + 300.f, y, "Reset " + std::string(TABS[mRulesTab]) + " to preset", SH_E, idx, "Put the whole table back as the preset has it", p);
        y -= ROW_H + 8.f;
    }
    return fValue(l + 10.f, r, y, "/" + key, key, ks, std::as_const(mRulesMerged)[key], !isRequired(rs, key), 1, p, TABS[mRulesTab]);
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// lore, rewards, handouts, learning, quizzes (§2.9)
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

F32 WolfAdventureStudioView::fTexture(F32 l, F32 r, F32 top, const std::string& label, const std::string& ptr, const LLSD& v, const Palette& p)
{
    // §2.9 "Handouts take a texture by drag or a picker, with a preview" (the same for portraits and decision images)
    const std::string cur = v.asString();
    LLUUID id;
    const bool has = !cur.empty() && LLUUID::validate(cur) && id.set(cur, false) && id.notNull();
    const F32 sz = 64.f;
    F32 cl = l;
    fRow(l, r, top, label, cl);
    // the preview, drawn from the texture fetch like any UI image
    chamferEdge(cl, top - sz, cl + sz, top, 3.f, 1.f, p.mDim);
    if (has)
    {
        LLViewerFetchedTexture* tex = LLViewerTextureManager::getFetchedTexture(id, FTT_DEFAULT, MIPMAP_YES, LLGLTexture::BOOST_UI);
        if (tex)
        {
            tex->addTextureStats((F32)(sz * sz));
            gl_draw_scaled_image((S32)cl + 2, (S32)(top - sz) + 2, (S32)sz - 4, (S32)sz - 4, tex);
        }
    }
    else textL(LLFontGL::getFontSansSerifSmall(), "none", cl + sz * 0.5f, top - sz * 0.5f, p.mDim, LLFontGL::HCENTER, LLFontGL::VCENTER);
    // the drop target: a texture dragged from inventory onto the preview (handleDragAndDrop)
    mDrops.push_back({ LLRect((S32)cl, (S32)top, (S32)(cl + sz), (S32)(top - sz)), ptr });
    const F32 x = cl + sz + 8.f;
    const S32 ic = act([this, ptr]() { chooseTexture(ptr); });
    btn(x, top - ROW_H, x + 110.f, top, "Choose...", SH_E, ic, "Choose a texture from your inventory (or drag one onto the picture)", p);
    if (has)
    {
        const S32 ix = act([this, ptr]() { docErase(0, ptr); });
        btn(x + 116.f, top - ROW_H, x + 206.f, top, "Remove", SH_E, ix, "No picture", p, true, BTN_DANGER);
    }
    F32 y = fText(x, r, top - ROW_H - 4.f, std::string(), ptr, v, 0, 36, 's', true, "The texture's asset id (full-permission textures only)", p);
    return llmin(y, top - sz - 8.f);
}

namespace
{
    /** "#rrggbb" (adventure_pack_v1.schema.json $defs/colour ^#[0-9a-fA-F]{6}$) as a colour; false when it is not one. */
    bool parseColour(const std::string& s, LLColor4& out)
    {
        if (s.size() != 7 || s[0] != '#') return false;
        for (size_t i = 1; i < 7; ++i) if (!isxdigit((unsigned char)s[i])) return false;
        const unsigned long v = strtoul(s.c_str() + 1, nullptr, 16);
        out = LLColor4(((v >> 16) & 0xff) / 255.f, ((v >> 8) & 0xff) / 255.f, (v & 0xff) / 255.f, 1.f);
        return true;
    }
}

F32 WolfAdventureStudioView::fColour(F32 l, F32 r, F32 top, const std::string& label, const std::string& ptr, const LLSD& v, S32 doc, bool optional,
                                     const Palette& p)
{
    F32 cl = l;
    fRow(l, r, top, label, cl);
    LLColor4 c;
    const bool ok = parseColour(v.asString(), c);
    // the swatch: the colour itself, framed, with "none" in words when there is no colour (never colour alone)
    chamferEdge(cl, top - ROW_H, cl + ROW_H, top, 2.f, 1.f, p.mDim);
    if (ok) rect(cl + 2.f, top - ROW_H + 2.f, cl + ROW_H - 2.f, top - 2.f, c);
    else textL(LLFontGL::getFontSansSerifSmall(), "-", cl + ROW_H * 0.5f, top - ROW_H * 0.5f, p.mDim, LLFontGL::HCENTER, LLFontGL::VCENTER);
    F32 y = fText(cl + ROW_H + 6.f, r, top, std::string(), ptr, v, doc, 7, 's', optional, label + ": a colour as #rrggbb, e.g. #f4a340", p);
    // [review] a colour that is not one says so beside it (the brain refuses it too)
    if (!v.asString().empty() && !ok) y = para(cl, r, y, "Type # and six digits or letters a-f, e.g. #f4a340.", DANGER_INK, true) - 4.f;
    return y;
}

void WolfAdventureStudioView::chooseTexture(const std::string& ptr)
{
    // Source: llpanelprofile.cpp onShowTexturePicker - the viewer's texture picker owned by this view, a dependent
    // of the Studio floater so it closes with it; TEXTURE_SELECT answers with the asset id.
    if (LLFloater* old = mTexPicker.get()) old->closeFloater();
    LLFloater* parent = getParentByType<LLFloater>();
    if (!parent) return;
    LLUUID cur;
    cur.set(sdAt(mDraftPack, ptr).asString(), false);
    LLFloaterTexturePicker* fp = new LLFloaterTexturePicker(this, cur, LLUUID::null, cur, false, true, "CHOOSE A PICTURE",
                                                            PERM_NONE, PERM_NONE, false, NULL, PICK_TEXTURE);
    mTexPicker = fp->getHandle();
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    fp->setOnFloaterCommitCallback([handle, ptr](LLTextureCtrl::ETexturePickOp op, LLPickerSource, const LLUUID& asset_id, const LLUUID&, const LLUUID&)
    {
        if (handle.isDead() || op != LLTextureCtrl::TEXTURE_SELECT) return;
        WolfAdventureStudioView* self = handle.get();
        if (asset_id.isNull()) self->docErase(0, ptr);
        else self->docSet(0, ptr, boost::json::value(asset_id.asString()));
    });
    fp->setLocalTextureEnabled(false);
    fp->setBakeTextureEnabled(false);
    fp->setCanApply(false, true, false);
    parent->addDependentFloater(mTexPicker);
    fp->openFloater();
    fp->setFocus(true);
}

F32 WolfAdventureStudioView::fQuiz(F32 l, F32 r, F32 top, const std::string& qp, const LLSD& q, const Palette& p)
{
    const boost::json::value& root = packSchema();
    const boost::json::value& qs = def(root, "quiz");
    F32 y = fObject(l, r, top, qp, qs, q, 0, p, { "id", "title", "passMark" });
    const boost::json::value& items = prop(qs, "items");
    const boost::json::value& is = items.is_object() && items.as_object().if_contains("items") ? items.as_object().at("items") : nullV();
    const std::string ip = ptrJoin(qp, "items");
    const LLSD& list = q["items"];
    const S32 n = list.isArray() ? (S32)list.size() : 0;
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    y = head(l, r, y, "Questions", p);
    for (S32 i = 0; i < n; ++i)
    {
        const LLSD& it = list[i];
        const std::string itp = ptrIndex(ip, i);
        y = cardHead(l, r, y, itp, llformat("%d. ", i + 1) + cap(it["prompt"].asString(), 60) + " (" + labelFor(it["type"].asString()) + ")", i, n, ip, true, p);
        if (!mOpen.count(itp)) continue;
        const F32 il = l + 14.f;
        y = fObject(il, r, y, itp, is, it, 0, p, { "id", "type", "prompt", "points", "source" });
        const std::string type = it["type"].asString();
        if (type == "choice")
        {
            y = fObject(il, r, y, itp, is, it, 0, p, { "choices", "maxChoices" });
            // §2.9 "the correct answers marked by a tick and the word 'correct'"
            y = head(il, r, y, "Which are correct", p);
            const LLSD& correct = it["correct"];
            for (const LLSD& c : llsd::inArray(it["choices"]))
            {
                const std::string cid = c["id"].asString();
                bool on = false;
                for (const LLSD& x : llsd::inArray(correct)) on = on || x.asString() == cid;
                const std::string cp = ptrJoin(itp, "correct");
                const S32 idx = act([this, cp, cid, on, correct]()
                {
                    boost::json::array out;
                    for (const LLSD& x : llsd::inArray(correct)) if (x.asString() != cid) out.push_back(boost::json::value(x.asString()));
                    if (!on) out.push_back(boost::json::value(cid));
                    docSet(0, cp, out);
                });
                chamferEdge(il, y - 18.f, il + 18.f, y, 2.f, 1.5f, p.mText);
                if (on) { lineW(il + 4.f, y - 9.f, il + 8.f, y - 14.f, 2.f, OK_INK); lineW(il + 8.f, y - 14.f, il + 15.f, y - 4.f, 2.f, OK_INK); }
                F32 tx = textL(small, cap(c["label"].asString(), 120) + " (" + cid + ")", il + 26.f, y - 9.f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, r - il - 110.f);
                if (on) textL(LLFontGL::getFontSansSerifSmallBold(), "correct", tx + 10.f, y - 9.f, OK_INK, LLFontGL::LEFT, LLFontGL::VCENTER);
                hit(il, y - 20.f, r, y, SH_E, idx, cap(c["label"].asString(), 60) + (on ? " - marked correct; press to unmark" : " - not marked correct; press to mark"));
                if (mKeyboardNav && hasFocus() && mHits.isFocused(SH_E, idx)) focusRing(il, y - 20.f, r, y, p);
                y -= 24.f;
            }
            if (!it["choices"].isArray() || it["choices"].size() == 0) y = para(il, r, y, "Add choices first, then tick the correct ones.", p.mDim, true);
            const std::string err = problemAt(ptrJoin(itp, "correct"));
            if (!err.empty()) y = para(il, r, y, err, DANGER_INK, true);
            y -= 6.f;
        }
        else if (type == "text") y = fObject(il, r, y, itp, is, it, 0, p, { "accept" });
        else y = para(il, r, y, "An essay: a teacher marks it in the Teacher Console.", p.mDim, true) - 6.f;
    }
    if (n < 20)
    {
        const boost::json::value* isp = &is;
        const std::string next = ptrIndex(ip, n);
        const S32 idx = act([this, ip, isp, next]() { docAppend(0, ip, makeDefault(packSchema(), *isp, "items", mDraftJson, std::string())); mOpen.insert(next); });
        btn(l, y - ROW_H, l + 160.f, y, "Add a question", SH_E, idx, "Add a question (at most 20)", p);
        y -= ROW_H + 8.f;
    }
    return y;
}

F32 WolfAdventureStudioView::pageLore(F32 l, F32 r, F32 top, const Palette& p)
{
    if (!openIsOwner()) return waiting(l, r, top, "Only the adventure's owner edits its lore.", p);
    if (mDraftJson.empty()) return waiting(l, r, top, "Opening the draft...", p);
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const boost::json::value& root = packSchema();
    F32 y = conflictBanner(l, r, top, p);
    // [WOLF ADVENTURE COMBAT 2026-10-17] Enemies and Battles (COMBAT_V1.md §1), drawn from the schema like the other lists
    static const char* TABS[9] = { "Story cards", "Source cards", "Encounters", "Rewards", "Handouts", "Learning", "Quizzes", "Enemies", "Battles" };
    static const char* KEYS[9] = { "storyCards", "sourceCards", "encounters", "parcels", "handouts", "", "quizzes", "enemies", "battles" };
    static const char* HELP[9] = {
        "Lore the characters know: read only when one of its keys comes up in a conversation.",
        "Cited facts for accuracy: every reply that uses one shows its citation.",
        "Groups of characters placed round a spot, with the props a bound chest rezzes.",
        "Rewards the story hands out: items, money, stats, inventory from a chest.",
        "Pictures, letters, maps and notes the players are shown.",
        "What the adventure and each scene teach: goals, key facts (source cards) and the quiz.",
        "Quizzes: choice, text and essay questions. Import QTI 3.0 brings items in from other tools.",
        "Your own enemies: a name, what they look like, their numbers (defaults are filled in), how they fight, and what they drop.",
        "Battles: which enemies, how many and where. A beat starts one with the Battle step; \"When they win\" moves the story on." };
    {
        F32 x = l;
        for (S32 i = 0; i < 9; ++i)
        {
            // a list this viewer's schema does not have yet (enemies and battles arrive with battles) is not offered
            if (KEYS[i][0] && prop(root, KEYS[i]).is_null()) { if (mLoreTab == i) mLoreTab = 0; continue; }
            const F32 bw = btnW(TABS[i]);
            if (x + bw > r) { x = l; y -= ROW_H + 4.f; }
            const S32 idx = act([this, i]() { mLoreTab = i; });
            btn(x, y - ROW_H, x + bw, y, TABS[i], SH_E, idx, std::string("Show ") + TABS[i], p, true, mLoreTab == i ? BTN_ACTIVE : 0);
            x += bw + 4.f;
        }
        y -= ROW_H + 8.f;
    }
    y = para(l, r, y, HELP[mLoreTab], p.mDim, true) - 6.f;
    if (mLoreTab == 5)
    {
        y = head(l, r, y, "The whole adventure", p);
        y = fValue(l, r, y, "/adventure/learning", "learning", prop(def(root, "adventure"), "learning"), std::as_const(mDraftPack)["adventure"]["learning"], true, 0, p, "Learning");
        const LLSD& acts = std::as_const(mDraftPack)["acts"];
        for (size_t a = 0; acts.isArray() && a < (size_t)acts.size(); ++a)
            for (size_t s = 0; s < (size_t)acts[(LLSD::Integer)a]["scenes"].size(); ++s)
            {
                const LLSD& sc = acts[(LLSD::Integer)a]["scenes"][(LLSD::Integer)s];
                const std::string sp = llformat("/acts/%d/scenes/%d", (S32)a, (S32)s);
                y = head(l, r, y, "Scene: " + cap(sc["title"].asString(), 60), p);
                y = fValue(l, r, y, sp + "/learning", "learning", prop(def(root, "scene"), "learning"), sc["learning"], true, 0, p, "Learning");
            }
        return y;
    }
    const std::string key = KEYS[mLoreTab];
    const std::string arr = "/" + key;
    if (mLoreTab == 6)
    {
        const S32 iq = act([this]() { importQti(); });
        btn(l, y - ROW_H, l + 180.f, y, "Import QTI 3.0...", SH_E, iq, "Add the questions of a QTI 3.0 content package (.zip, up to 10 MiB) as quizzes", p);
        y -= ROW_H + 8.f;
        const LLSD& qz = std::as_const(mDraftPack)["quizzes"];
        const S32 n = qz.isArray() ? (S32)qz.size() : 0;
        for (S32 i = 0; i < n; ++i)
        {
            const std::string qp = ptrIndex(arr, i);
            y = cardHead(l, r, y, qp, cap(qz[i]["title"].asString().empty() ? qz[i]["id"].asString() : qz[i]["title"].asString(), 60) +
                         llformat(" (%d questions)", qz[i]["items"].isArray() ? (S32)qz[i]["items"].size() : 0), i, n, arr, true, p);
            if (mOpen.count(qp)) y = fQuiz(l + 14.f, r, y, qp, qz[i], p);
        }
        const std::string next = ptrIndex(arr, n);
        const S32 ia = act([this, arr, next]() { docAppend(0, arr, makeDefault(packSchema(), def(packSchema(), "quiz"), "quizzes", mDraftJson, std::string())); mOpen.insert(next); });
        btn(l, y - ROW_H, l + 140.f, y, "Add a quiz", SH_E, ia, "Add a quiz", p, n < 200);
        return y - ROW_H - 8.f;
    }
    return fArray(l, r, y, arr, key, prop(root, key), std::as_const(mDraftPack)[key], 0, p, TABS[mLoreTab]);
}

void WolfAdventureStudioView::importQti()
{
    // BRAIN_API_V1.md §5.1 import {adventure?, kind:"qti3", name, content (zip base64)} -> {pack, problems, report};
    // "cast or quiz imports add to the open draft". §11.3: the brain refuses a package over 10 MiB.
    static const U64 QTI_MAX_BYTES = 10ull * 1024ull * 1024ull;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    LLFilePickerReplyThread::startPicker(
        [handle](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (handle.isDead() || files.empty() || files.front().empty()) return;
            WolfAdventureStudioView* self = handle.get();
            const std::string path = files.front();
            llifstream in(path.c_str(), std::ios::in | std::ios::binary);
            if (!in.is_open()) { self->setStatus("That file could not be opened.", true); return; }
            in.seekg(0, std::ios::end);
            const std::streamoff size = in.tellg();
            if (size <= 0 || (U64)size > QTI_MAX_BYTES) { self->setStatus("A QTI package may be at most 10 MiB.", true); return; }
            in.seekg(0, std::ios::beg);
            std::vector<U8> bytes((size_t)size);
            in.read((char*)bytes.data(), size);
            // a zip starts "PK\3\4" (APPNOTE.TXT 4.3.7 local file header signature 0x04034b50)
            if (bytes.size() < 4 || bytes[0] != 'P' || bytes[1] != 'K' || bytes[2] != 3 || bytes[3] != 4)
            {
                self->setStatus("That is not a QTI content package (a .zip file).", true);
                return;
            }
            LLSD body;
            body["adventure"] = self->mOpenId;
            body["kind"] = "qti3";
            body["name"] = cap(gDirUtilp->getBaseFileName(path), 64);
            body["content"] = LLBase64::encode(bytes.data(), bytes.size());
            const std::string adv = self->mOpenId;
            self->call("import", body, [adv](WolfAdventureStudioView* self, const LLSD& reply)
            {
                if (self->mOpenId != adv) return;
                boost::system::error_code ec;
                boost::json::value pk = boost::json::parse(reply["pack_json"].asString(), ec);
                const boost::json::value* qz = ec ? nullptr : jAt(pk, "/quizzes", false);
                std::string report;
                for (const LLSD& line : llsd::inArray(reply["report"])) if (report.size() < 300) report += (report.empty() ? "" : " ") + cap(line.asString(), 200);
                if (!qz || !qz->is_array() || qz->as_array().empty())
                {
                    self->setStatus("No questions came in." + (report.empty() ? std::string() : " " + report), true);
                    return;
                }
                // the quizzes added to the open draft, ids kept unique in the pack
                boost::json::array add = qz->as_array();
                S32 items = 0;
                std::string seen = self->mDraftJson;
                for (boost::json::value& q : add)
                {
                    if (!q.is_object()) continue;
                    const std::string base = q.as_object().contains("id") && q.as_object().at("id").is_string() ? std::string(q.as_object().at("id").as_string().c_str()) : std::string("quiz");
                    const std::string nid = uniqueId(base, seen);
                    seen += "\"id\":\"" + nid + "\"";
                    q.as_object()["id"] = nid;
                    if (const boost::json::value* its = q.as_object().if_contains("items")) if (its->is_array()) items += (S32)its->as_array().size();
                }
                if (!self->editDoc([&add](boost::json::value& root)
                {
                    boost::json::value* to = jAt(root, "/quizzes", true);
                    if (!asList(to)) return false;
                    for (const boost::json::value& q : add) to->as_array().push_back(q);
                    return true;
                }))
                {
                    self->setStatus("The questions could not be added to the draft (its quizzes are not a list).", true);
                    return;
                }
                self->setStatus(llformat("Added %d quiz%s with %d questions.", (S32)add.size(), add.size() == 1 ? "" : "zes", items) + (report.empty() ? std::string() : " " + report), false);
            }, "Importing the QTI package...");
        },
        LLFilePicker::FFLOAD_ALL, false);
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// test play (§2.7)
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

void WolfAdventureStudioView::testStart(const std::string& beat_in, std::function<void()> then)
{
    std::string beat = beat_in;
    if (beat.empty())
    {
        const auto beats = draftBeats();
        if (!beats.empty()) beat = beats.front().first;
    }
    if (beat.empty()) { mTestError = "There is no box to play yet: build your game first."; return; }
    // the draft as it is now: an unsaved change is saved first so the test plays what the author sees
    if (mDirtyAt > 0.0 || mSaving)
    {
        if (!mSaving) saveDraftNow();
        mTestError = "Saving your last changes first - press Play from the start again in a moment.";
        return;
    }
    mTestBeat = beat;
    mTestError.clear();
    LLSD body;
    body["adventure"] = mOpenId;
    body["beat"] = beat;
    body["ai"] = mTestNoAi ? "none" : "ai";
    const std::string adv = mOpenId;
    ++mBusy;
    setStatus("Starting the test...", false);
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // BRAIN_API_V1.md §5.5 test_start {adventure, beat, ai} -> {session, region, regionName, here}; refused with the
    // readable reason (and the problems when the draft has errors)
    WolfAdventure::api("test_start", body, [handle, adv, then](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        self->mBusy = llmax(0, self->mBusy - 1);
        if (self->mOpenId != adv) return;
        if (!ok)
        {
            self->mTestError = error;
            if (reply["problems"].isArray()) self->setProblems(reply["problems"]);
            self->setStatus("The test did not start: " + error, true);
            return;
        }
        self->mTestSession = reply["session"].asString();
        self->mTestRegionName = cap(reply["regionName"].asString(), 64);
        self->mTestState = LLSD();
        self->mTestFlags = LLSD::emptyMap();
        self->setStatus("Test started - only you are in it.", false);
        if (then) then();   // [SIMPLE STUDIO 2026-10-17] page 3's Play it now opens the test page (self is alive: the handle said so)
        self->pollTest(true);
        // §2.7 "when the scene's region is elsewhere it asks to teleport"
        if (reply["here"].asInteger() == 0 && !self->mTestRegionName.empty())
        {
            self->mConfirmKeyA = self->mTestRegionName;
            self->ask(SC_TEST_TELEPORT, 0);
        }
    });
}

void WolfAdventureStudioView::testOp(const std::string& action, const LLSD& extra, const std::string& doing)
{
    if (mTestSession.empty()) { mTestError = "Start a test first."; return; }
    LLSD body = extra.isMap() ? extra : LLSD::emptyMap();
    body["session"] = mTestSession;
    const std::string sess = mTestSession;
    // BRAIN_API_V1.md §5.5 test_reset / test_end {session} -> {}; test_fire {session, trigger} / test_set {session, flag, value}
    // -> {log:{op, trigger|flag, fired|value, beat}}
    call(action, body, [action, sess](WolfAdventureStudioView* self, const LLSD& reply)
    {
        if (self->mTestSession != sess) return;
        self->mTestError.clear();
        const LLSD& lg = reply["log"];
        if (action == "test_end")
        {
            self->mTestSession.clear();
            self->mTestState = LLSD();
            self->mTestFlags = LLSD::emptyMap();
            self->setStatus("Test ended. Nothing it did was real: no rewards were given.", false);
            return;
        }
        if (action == "test_reset") self->setStatus("Test reset to its first box.", false);
        else if (action == "test_fire")
            self->setStatus(lg["fired"].asInteger() ? "Fired \"" + lg["trigger"].asString() + "\"; the box is now " + lg["beat"].asString() + "."
                                                    : "\"" + lg["trigger"].asString() + "\" did not fire (its Only if is not met, or it fires once and already has).", false);
        else if (action == "test_set") self->setStatus(lg["flag"].asString() + " is now " + valueText(lg["value"]) + ".", false);
        self->pollTest(true);
    }, doing);
}

void WolfAdventureStudioView::pollTest(bool force)
{
    if (mTestSession.empty() || mTestLoading) return;
    if (!force && now() < mTestPollAt) return;
    mTestLoading = true;
    mTestPollAt = now() + 2.0;   // §2.7 the overlay polls every 2 s while it shows (lead's test_state contract)
    LLSD body;
    body["session"] = mTestSession;
    const std::string sess = mTestSession;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    WolfAdventure::api("test_state", body, [handle, sess](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        self->mTestLoading = false;
        if (self->mTestSession != sess) return;
        if (!ok)
        {
            self->mTestError = "The test's state could not be read: " + error;
            self->mTestPollAt = now() + 6.0;
            return;
        }
        if (self->mTestError.rfind("The test's state", 0) == 0) self->mTestError.clear();
        self->mTestState = reply;
        LLSD flags = LLSD::emptyMap();
        for (const LLSD& f : llsd::inArray(reply["flags"])) flags[f["id"].asString()] = f["value"];
        self->mTestFlags = flags;
    });
}

F32 WolfAdventureStudioView::pageTest(F32 l, F32 r, F32 top, const Palette& p)
{
    if (!openIsOwner()) return waiting(l, r, top, "Only the game's owner can play it to test it.", p);
    if (mDraftJson.empty()) return waiting(l, r, top, "Opening your game...", p);
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    F32 y = conflictBanner(l, r, top, p);
    // the controls on the left, the debug overlay plate on the right (§2.7 "a plate at the right")
    const F32 split = l + llmax(220.f, (r - l) * 0.36f);
    F32 ly = head(l, split - 10.f, y, "Test Play", p);
    ly = para(l, split - 10.f, ly, "Plays your game for you alone, from the box you choose, with its characters on your land. Test games never give real rewards.", p.mDim, true) - 6.f;
    {
        std::string label = mTestBeat;
        for (const auto& b : draftBeats()) if (b.first == mTestBeat) label = b.second;
        if (label.empty()) label = "(the first box)";
        textL(small, "From the box", l, ly - 4.f, p.mText);
        ly -= slh + 4.f;
        const S32 ib = act([this]()
        {
            openPicker("Play from the box", draftBeats(), [this](const std::string& v) { if (!v.empty()) mTestBeat = v; }, "There is no box yet: build your game first.");
        });
        btn(l, ly - ROW_H, split - 10.f, ly, cap(label, 40), SH_E, ib, "Choose the box the test starts at", p);
        ly -= ROW_H + 8.f;
    }
    {
        // §2.7 "Preview without AI runs the test in non-AI mode, so the story is proven both ways"
        const S32 it = act([this]() { mTestNoAi = !mTestNoAi; });
        chamferEdge(l, ly - 18.f, l + 18.f, ly, 2.f, 1.5f, p.mText);
        if (mTestNoAi) { lineW(l + 4.f, ly - 9.f, l + 8.f, ly - 14.f, 2.f, OK_INK); lineW(l + 8.f, ly - 14.f, l + 15.f, ly - 4.f, 2.f, OK_INK); }
        textL(small, mTestNoAi ? "Preview without AI - on" : "Preview without AI - off", l + 26.f, ly - 9.f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER);
        hit(l, ly - 20.f, split - 10.f, ly, SH_E, it, mTestNoAi ? "Written lines only (on); press for AI" : "With AI (Preview without AI is off); press for written lines only");
        if (mKeyboardNav && hasFocus() && mHits.isFocused(SH_E, it)) focusRing(l, ly - 20.f, split - 10.f, ly, p);
        ly -= 28.f;
    }
    {
        const bool running = !mTestSession.empty();
        const S32 is = act([this]() { testStart(mTestBeat); });
        btn(l, ly - ROW_H, split - 10.f, ly, running ? "Start again from this box" : "Start test", SH_E, is, "Start playing your game to test it (your earlier test of this game ends first)", p, mBusy == 0, BTN_PRIMARY);
        ly -= ROW_H + 6.f;
        const S32 ir = act([this]() { testOp("test_reset", LLSD::emptyMap(), "Resetting the test..."); });
        btn(l, ly - ROW_H, split - 10.f, ly, "Reset test", SH_E, ir, "Clear the test's flags and go back to its first box", p, running);
        ly -= ROW_H + 6.f;
        const S32 ie = act([this]() { testOp("test_end", LLSD::emptyMap(), "Ending the test..."); });
        btn(l, ly - ROW_H, split - 10.f, ly, "End test", SH_E, ie, "End the test: its characters leave and its state is cleared", p, running, BTN_DANGER);
        ly -= ROW_H + 8.f;
        if (running && !mTestRegionName.empty())
        {
            LLViewerRegion* rgn = gAgent.getRegion();
            if (!rgn || rgn->getName() != mTestRegionName)
            {
                ly = para(l, split - 10.f, ly, "This part plays on " + mTestRegionName + ".", p.mDim, true);
                const S32 itp = act([this]() { mConfirmKeyA = mTestRegionName; ask(SC_TEST_TELEPORT, 0); });
                btn(l, ly - ROW_H, split - 10.f, ly, "Teleport there...", SH_E, itp, "Teleport to " + mTestRegionName + " (asks first)", p);
                ly -= ROW_H + 8.f;
            }
        }
    }
    if (!mTestError.empty())
    {
        textL(bold, "Problem:", l, ly, DANGER_INK);
        ly = para(l, split - 10.f, ly - slh, mTestError, DANGER_INK, true) - 6.f;
    }
    // the plate
    const F32 pl = split, pr = r;
    F32 py = y;
    if (mTestSession.empty())
    {
        py = para(pl + 10.f, pr - 10.f, py - 10.f, "Start a test to see what is going on: where the story is, its flags (change them here), what can happen now with Fire buttons, the last 20 events, the timers, and the AI meter.", p.mDim, true);
        return llmin(ly, py) - 8.f;
    }
    const F32 plate_top = py;
    py -= 10.f;
    const LLSD& st = mTestState;
    py = head(pl + 10.f, pr - 10.f, py, "Where the story is", p);
    if (!st.isMap()) py = para(pl + 10.f, pr - 10.f, py, mTestLoading ? "Reading the test..." : "Waiting for the test's state...", p.mDim, true);
    else
    {
        py = para(pl + 10.f, pr - 10.f, py, "Part " + st["act"].asString() + ", " + st["scene"].asString() + "  >  Box " + st["beat"].asString(), p.mText, true);
        if (!st["objective"].asString().empty()) py = para(pl + 10.f, pr - 10.f, py, "Objective: " + cap(st["objective"].asString(), 240), p.mDim, true);
        // [GAME CARD 2026-10-18] the ending's kind (adventure_pack_v1 ending.kind win | lose | other) says how it ended, as the web Studio
        const std::string endId = st["ending"].isMap() ? st["ending"]["id"].asString() : st["ending"].asString();
        if (!endId.empty())
        {
            std::string kind;
            for (const LLSD& e : llsd::inArray(std::as_const(mDraftPack)["endings"])) if (e["id"].asString() == endId) kind = e["kind"].asString();
            py = para(pl + 10.f, pr - 10.f, py, kind == "win" ? std::string("The story has ended: you won.") : kind == "lose" ? std::string("The story has ended: you lost.")
                                                                                                               : std::string("The story has ended."), kind == "win" ? OK_INK : WARN_INK, true);
        }
        py -= 6.f;
        // flags, editable (doc 2: test_set)
        py = head(pl + 10.f, pr - 10.f, py, "Flags", p);
        if (!st["flags"].isArray() || st["flags"].size() == 0) py = para(pl + 10.f, pr - 10.f, py, "This pack has no flags.", p.mDim, true);
        for (const LLSD& f : llsd::inArray(st["flags"]))
        {
            const std::string fid = f["id"].asString(), type = f["type"].asString();
            const std::string label = fid + " (" + f["scope"].asString() + ")";
            if (type == "bool")
            {
                const bool on = f["value"].asBoolean();
                const S32 idx = act([this, fid, on]()
                {
                    LLSD extra;
                    extra["flag"] = fid;
                    extra["value"] = !on;
                    testOp("test_set", extra, "Setting " + fid + "...");
                });
                F32 cl = pl + 10.f;
                fRow(pl + 10.f, pr - 10.f, py, label, cl);
                btn(cl, py - ROW_H, cl + 90.f, py, on ? "Yes" : "No", SH_E, idx, label + (on ? " - now Yes; press for No" : " - now No; press for Yes"), p, true, on ? BTN_ACTIVE : 0);
                py -= ROW_H + 4.f;
            }
            else py = fText(pl + 10.f, pr - 10.f, py, label, "/" + ptrEscape(fid), f["value"], 2, 240, type == "int" ? 'i' : 's', false,
                            "Type the value and press Enter: it is set on the test", p);
        }
        py -= 4.f;
        // triggers armed on this beat, with Fire
        py = head(pl + 10.f, pr - 10.f, py, "What can happen in this box", p);
        if (!st["triggers"].isArray() || st["triggers"].size() == 0) py = para(pl + 10.f, pr - 10.f, py, "Nothing is set to happen in this box yet.", p.mDim, true);
        for (const LLSD& t : llsd::inArray(st["triggers"]))
        {
            const std::string tid = t["id"].asString();
            const TriggerType* tt = triggerType(t["on"].asString());
            std::string text = (t["label"].asString().empty() ? tid : cap(t["label"].asString(), 60)) + " - " + (tt ? std::string(tt->mSentence) : t["on"].asString());
            if (t["fired"].asInteger()) text += t["once"].asInteger() ? " (fired; once)" : " (fired)";
            const S32 idx = act([this, tid]()
            {
                LLSD extra;
                extra["trigger"] = tid;
                testOp("test_fire", extra, "Firing " + tid + "...");
            });
            textL(small, text, pl + 10.f, py - ROW_H * 0.5f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, (pr - pl) * 0.45f);
            // [WORDING 2026-10-18] the button says what it makes happen, as the web Studio's test play (never the trigger's code)
            static const std::map<std::string, std::string> WORD = { { "arrive", "Arrive at the place" }, { "talk", "Talk to the character" },
                { "touch", "Touch the object" }, { "say", "Say the words" }, { "choose", "Make the choice" }, { "timer", "Wait out the timer" },
                { "flag", "Something comes true" }, { "ready", "Everyone is ready" } };
            const auto wit = WORD.find(t["on"].asString());
            const std::string fw = wit != WORD.end() ? wit->second : "Make it happen: " + triggerWords(t, mDraftPack);
            const F32 fbw = llmin((pr - pl) * 0.5f, btnW(fw));
            btn(pr - 10.f - fbw, py - ROW_H, pr - 10.f, py, fw, SH_E, idx, "Make \"" + tid + "\" happen without walking there", p, !(t["fired"].asInteger() && t["once"].asInteger()));
            py -= ROW_H + 4.f;
        }
        py -= 4.f;
        // timers
        py = head(pl + 10.f, pr - 10.f, py, "Timers", p);
        if (!st["timers"].isArray() || st["timers"].size() == 0) py = para(pl + 10.f, pr - 10.f, py, "None running.", p.mDim, true);
        for (const LLSD& t : llsd::inArray(st["timers"]))
            py = para(pl + 10.f, pr - 10.f, py, labelFor(t["kind"].asString()) + llformat(" in %d s", t["in"].asInteger()) + (t["what"].asString().empty() ? std::string() : ": " + cap(t["what"].asString(), 80)), p.mText, true);
        py -= 4.f;
        // the AI meter
        py = head(pl + 10.f, pr - 10.f, py, "AI", p);
        py = para(pl + 10.f, pr - 10.f, py, cap(st["ai"]["modeText"].asString(), 120) + llformat("  -  today %.2f credits", st["ai"]["todayCredits"].asReal()), p.mText, true) - 4.f;
        // the last 20 events (newest first)
        py = head(pl + 10.f, pr - 10.f, py, "Last events", p);
        S32 shown_events = 0;
        for (const LLSD& e : llsd::inArray(st["log"]))   // newest first, as test_state sends them
        {
            if (++shown_events > 20) break;   // §2.7 "the last 20 events"
            std::string who = e["name"].asString().empty() ? e["source"].asString() : e["name"].asString();
            py = para(pl + 10.f, pr - 10.f, py, cap(who, 40) + ": " + e["type"].asString() + (e["text"].asString().empty() ? std::string() : " - " + cap(e["text"].asString(), 200)), p.mDim, true);
        }
    }
    py -= 10.f;
    // the plate's rim, drawn round what it holds
    chamferEdge(pl, py, pr, plate_top, 6.f, 1.5f, p.mAccent);
    return llmin(ly, py) - 8.f;
}

void WolfAdventureStudioView::pickVoice(S32 cast_index, const std::string& vid)
{
    const LLSD& cast = std::as_const(mDraftPack)["cast"];
    if (!cast.isArray() || cast_index < 0 || cast_index >= (S32)cast.size()) { setStatus(STALE, true); return; }
    const std::string vp = ptrIndex("/cast", cast_index) + "/voice";
    if (vid.empty()) { docErase(0, vp); return; }
    // a premium voice (lead's P3 voices contract: id "eleven:<voice_id>", premium:1, price.microPerChar > 0) costs
    // money per character spoken: the first one chosen in this Studio asks first, with the price in words
    for (const LLSD& vo : llsd::inArray(mVoices))
    {
        if (vo["id"].asString() != vid || !vo["premium"].asBoolean() || mPremiumOk) continue;
        mPremiumCast = cast_index;
        mPremiumVoice = vid;
        mConfirmKeyA = cap(vo["label"].asString(), 60);
        mConfirmKeyB = llformat("%.2f", vo["price"]["microPerChar"].asReal() * 100.0 / 10000.0);
        ask(SC_PREMIUM, 0);
        return;
    }
    const bool had_rate = cast[cast_index]["voice"].has("rate");
    if (!editDoc([&vp, &vid, had_rate](boost::json::value& root)
    {
        boost::json::value* v = jAt(root, vp, true);
        if (!v || !v->is_object()) return false;
        v->as_object()["id"] = vid;
        if (!had_rate) v->as_object()["rate"] = 1.0;
        return true;
    }) && !mDraftJson.empty()) setStatus(STALE, true);
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// Use what I'm wearing (§2.3; BRAIN §5.5 save_look)
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

void WolfAdventureStudioView::saveLook(const std::string& cid)
{
    if (!mLookCast.empty()) return;
    if (mDirtyAt > 0.0 || mSaving)
    {
        // the brain writes the look into the saved draft: unsaved changes go first so neither is lost
        if (!mSaving) saveDraftNow();
        setStatus("Saving your last changes first - press Use what I'm wearing again in a moment.", false);
        return;
    }
    std::string before;
    for (const LLSD& c : llsd::inArray(std::as_const(mDraftPack)["cast"])) if (c["id"].asString() == cid) before = c["look"]["appearance"].asString();
    LLSD body;
    body["adventure"] = mOpenId;
    body["cast"] = cid;
    mLookCast = cid;
    mLookBefore = before;
    const std::string adv = mOpenId;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // -> {queued:1}; refused unless the owner stands on a region the adventure is enabled on
    WolfAdventure::api("save_look", body, [handle, adv](bool ok, const LLSD&, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        if (self->mOpenId != adv) { self->mLookCast.clear(); return; }
        if (!ok)
        {
            self->mLookCast.clear();
            self->setStatus("Your outfit was not saved: " + error, true);
            return;
        }
        self->mLookTries = 0;
        self->mLookReloadAt = now() + 5.0;   // the region saves the outfit, then the brain writes it into the draft
        self->setStatus("Saving what you are wearing - your region makes the appearance notecard...", false);
    });
}

void WolfAdventureStudioView::lookReload()
{
    mLookReloadAt = 0.0;
    if (mLookCast.empty() || mOpenId.empty()) return;
    LLSD body;
    body["adventure"] = mOpenId;
    const std::string adv = mOpenId, cid = mLookCast;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    WolfAdventure::api("get_draft", body, [handle, adv, cid](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        if (self->mOpenId != adv || self->mLookCast != cid) return;
        std::string now_look, name;
        for (const LLSD& c : llsd::inArray(reply["pack"]["cast"]))
            if (c["id"].asString() == cid) { now_look = c["look"]["appearance"].asString(); name = cap(c["first"].asString() + " " + c["last"].asString(), 60); }
        if (!ok || now_look.empty() || now_look == self->mLookBefore)
        {
            if (++self->mLookTries < 4) { self->mLookReloadAt = now() + 5.0; return; }
            self->mLookCast.clear();
            self->setStatus(ok ? std::string("Your region has not reported the outfit yet. Look on the Cast page again in a minute, or try once more.")
                               : "The draft could not be read back: " + error, true);
            return;
        }
        self->mLookCast.clear();
        if (self->mDirtyAt > 0.0 || self->mSaving)
        {
            // changes made meanwhile: keep them, take the brain's etag, and write the look into them
            self->mEtag = reply["etag"].asString();
            boost::json::value a(now_look);
            S32 idx = -1;
            for (size_t i = 0; i < (size_t)std::as_const(self->mDraftPack)["cast"].size(); ++i) if (std::as_const(self->mDraftPack)["cast"][(LLSD::Integer)i]["id"].asString() == cid) idx = (S32)i;
            if (idx >= 0) self->docSet(0, llformat("/cast/%d/look/appearance", idx), a);
        }
        else
        {
            self->setDraftText(reply["pack_json"].asString(), reply["pack"]);
            self->mEtag = reply["etag"].asString();
            self->setProblems(reply["problems"]);
            self->mRulesGen = 0;
        }
        self->setStatus("Saved what you are wearing as " + name + "'s look.", false);
    });
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// the picker, problems, and the window's hooks
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

namespace
{
    std::string lower(const std::string& s) { std::string o = s; LLStringUtil::toLower(o); return o; }
    const F32 PICK_ROW = 26.f;
}

std::vector<S32> WolfAdventureStudioView::pickerRows() const
{
    std::vector<S32> rows;
    const std::string f = lower(mPickerFilter);
    for (S32 i = 0; i < (S32)mPicker.mOpts.size(); ++i)
    {
        const auto& o = mPicker.mOpts[i];
        if (o.first == "~hdr") { if (f.empty()) rows.push_back(i); continue; }
        if (f.empty() || lower(o.second).find(f) != std::string::npos || lower(o.first).find(f) != std::string::npos) rows.push_back(i);
    }
    return rows;
}

void WolfAdventureStudioView::pickerChoose(S32 opt)
{
    if (opt < 0 || opt >= (S32)mPicker.mOpts.size() || mPicker.mOpts[opt].first == "~hdr") return;
    const std::string v = mPicker.mOpts[opt].first;
    const std::function<void(const std::string&)> set = mPicker.mSet;
    closePicker();
    if (set) set(v);
}

void WolfAdventureStudioView::closePicker()
{
    mPicker.mOn = false;
    mPicker.mOpts.clear();
    mPicker.mSet = nullptr;
    mPickerFilter.clear();
    mPickerScroll = 0.f;
    mPickerSel = 0;
}

void WolfAdventureStudioView::drawPicker(const Palette& p)
{
    // a list chosen from inside the window (§1 "never plain form floaters"), modal like the confirmations
    mHits.clear();
    const F32 W = (F32)getRect().getWidth(), H = (F32)getRect().getHeight();
    rect(0.f, 0.f, W, H, LLColor4(0.f, 0.f, 0.f, 0.55f));
    const S32 ib = act([this]() { closePicker(); });
    mHits.add(0.f, 0.f, W, H, SH_E, ib, "Close the list (Escape)", false);
    const std::vector<S32> rows = pickerRows();
    const LLFontGL* f = LLFontGL::getFontSansSerif();
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const F32 pw = llmin(W - 40.f, 500.f);
    const F32 list_h = llclamp((F32)llmax((size_t)1, rows.size()) * PICK_ROW, PICK_ROW, H - 190.f);
    const F32 ph = 16.f + (F32)display(18.f)->getLineHeight() + 8.f + (F32)small->getLineHeight() + 8.f + list_h + 12.f + 30.f + 14.f;
    const F32 pl = (W - pw) * 0.5f, pt = (H + ph) * 0.5f, pb = pt - ph;
    frame(pl, pb, pl + pw, pt, p, 1.f, 10.f);
    F32 y = pt - 16.f;
    textL(display(18.f), mPicker.mTitle, pl + 14.f, y, p.mText, LLFontGL::LEFT, LLFontGL::TOP, pw - 28.f);
    y -= (F32)display(18.f)->getLineHeight() + 8.f;
    textL(small, mPickerFilter.empty() ? std::string("Type to filter the list; Up / Down and Enter choose") : "Filter: " + mPickerFilter,
          pl + 14.f, y, mPickerFilter.empty() ? p.mDim : p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, pw - 28.f);
    y -= (F32)small->getLineHeight() + 8.f;
    const F32 lt = y, lb = y - list_h;
    // keep the chosen row in view (keyboard)
    mPickerSel = rows.empty() ? 0 : llclamp(mPickerSel, 0, (S32)rows.size() - 1);
    const F32 max_scroll = llmax(0.f, (F32)rows.size() * PICK_ROW - list_h);
    if (mPickerSelMoved)
    {
        const F32 sel_top = mPickerSel * PICK_ROW;
        if (sel_top < mPickerScroll) mPickerScroll = sel_top;
        if (sel_top + PICK_ROW > mPickerScroll + list_h) mPickerScroll = sel_top + PICK_ROW - list_h;
        mPickerSelMoved = false;
    }
    mPickerScroll = llclamp(mPickerScroll, 0.f, max_scroll);
    {
        LLLocalClipRect clip(LLRect((S32)pl + 4, (S32)lt, (S32)(pl + pw) - 4, (S32)lb));
        if (rows.empty())
            textL(f, mPicker.mOpts.empty() && !mPicker.mEmpty.empty() ? mPicker.mEmpty : std::string("Nothing matches."), pl + 14.f, lt - 4.f, p.mDim,
                  LLFontGL::LEFT, LLFontGL::TOP, pw - 28.f);
        for (S32 k = 0; k < (S32)rows.size(); ++k)
        {
            const F32 rt = lt - k * PICK_ROW + mPickerScroll, rb = rt - PICK_ROW;
            if (rb > lt || rt < lb) continue;
            const auto& o = mPicker.mOpts[rows[k]];
            if (o.first == "~hdr")
            {
                textL(LLFontGL::getFontSansSerifSmallBold(), o.second, pl + 14.f, (rt + rb) * 0.5f, p.mAccent, LLFontGL::LEFT, LLFontGL::VCENTER, pw - 28.f);
                continue;
            }
            const S32 opt = rows[k];
            const S32 idx = act([this, opt]() { pickerChoose(opt); });
            const bool sel = k == mPickerSel;
            if (sel || mHits.isHover(SH_E, idx)) rect(pl + 8.f, rb + 1.f, pl + pw - 8.f, rt - 1.f, fade(p.mAccent, sel ? 0.28f : 0.16f));
            textL(f, o.second, pl + 16.f, (rt + rb) * 0.5f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, pw - 32.f);
            if (sel && mKeyboardNav) focusRing(pl + 8.f, rb + 1.f, pl + pw - 8.f, rt - 1.f, p);
            mHits.add(pl + 8.f, llmax(rb, lb), pl + pw - 8.f, llmin(rt, lt), SH_E, idx, o.second, false);
        }
    }
    if (max_scroll > 0.f)
    {
        const F32 th = llmax(20.f, list_h * list_h / ((F32)rows.size() * PICK_ROW)), ty = lt - (list_h - th) * (mPickerScroll / max_scroll);
        rect(pl + pw - 8.f, lb, pl + pw - 4.f, lt, fade(p.mTrack, 0.8f));
        rect(pl + pw - 8.f, ty - th, pl + pw - 4.f, ty, fade(p.mDim, 0.8f));
    }
    const S32 ic = act([this]() { closePicker(); });
    const F32 bb = pb + 14.f;
    button(pl + pw - 164.f, bb, pl + pw - 14.f, bb + 30.f, "Cancel", p, flagsFor(SH_E, ic, true) | BTN_PRIMARY);
    mHits.add(pl + pw - 164.f, bb, pl + pw - 14.f, bb + 30.f, SH_E, ic, "Close the list without choosing (Escape)");
}

void WolfAdventureStudioView::gotoProblem(const std::string& path)
{
    // STUDIO §2.2 "Clicking one opens its JSON pointer's editor and focuses the field"
    const std::vector<std::string> t = ptrTokens(path);
    if (t.empty()) return;
    commitEdit();
    mField = F_NONE;
    const std::string& top = t[0];
    // every card along the way opens
    std::string acc;
    for (const std::string& tok : t) { acc = ptrJoin(acc, tok); mOpen.insert(acc); }
    EPage page = SP_OUTLINE;
    if (top == "acts")
    {
        page = SP_STORY;
        if (t.size() >= 6 && t[2] == "scenes" && t[4] == "beats") mSel = "/acts/" + t[1] + "/scenes/" + t[3] + "/beats/" + t[5];
        else if (t.size() >= 4 && t[2] == "scenes") mSel = "/acts/" + t[1] + "/scenes/" + t[3];
        else if (t.size() >= 2) mSel = "/acts/" + t[1];
        if (t.size() >= 6 && t[2] == "scenes" && t[4] == "beats" && t.size() == 6) page = SP_OUTLINE;   // a beat as a whole (a dead end): the flow strip
    }
    // [WOLF ADVENTURE AUDIT 2026-10-17] the adventure's learning is edited on Lore > Learning (the Outline skips it)
    else if (top == "adventure" && t.size() >= 2 && t[1] == "learning") { page = SP_LORE; mLoreTab = 5; }
    else if (top == "cast") { page = SP_CASTED; if (t.size() >= 2 && tokIndex(t[1]) >= 0) mCastSel2 = tokIndex(t[1]); }
    else if (top == "places")
    {
        page = SP_PLACES;
        static const char* KEYS[4] = { "spots", "areas", "paths", "chests" };
        for (S32 i = 0; i < 4; ++i) if (t.size() >= 2 && t[1] == KEYS[i]) mPlaceTab = i;
    }
    else if (top == "rules") { page = SP_RULES; mRulesTab = 0; }
    else if (top == "state") { page = SP_RULES; mRulesTab = 8; }
    else if (top == "ai") page = SP_AI;
    else
    {
        static const char* KEYS[9] = { "storyCards", "sourceCards", "encounters", "parcels", "handouts", "", "quizzes", "enemies", "battles" };
        for (S32 i = 0; i < 9; ++i) if (top == KEYS[i]) { page = SP_LORE; mLoreTab = i; }
    }
    mPage = page;
    mScroll[mPage] = 0.f;
    mFocusPtr = path;
    mFocusScroll = true;
    mFocusAt = now();
}

F32 WolfAdventureStudioView::conflictBanner(F32 l, F32 r, F32 top, const Palette& p)
{
    // STUDIO §1 "Changed in another window: Reload / Keep mine", on every page that edits the draft
    if (!mConflict) return top;
    F32 y = para(l, r, top, "Your game was changed in another window.", DANGER_INK) - 4.f;
    btn(l, y - 26.f, l + 130.f, y, "Reload", SH_CONFLICT_RELOAD, 0, "Throw your changes away and load the other window's version", p);
    btn(l + 140.f, y - 26.f, l + 270.f, y, "Keep mine", SH_CONFLICT_KEEP, 0, "Save your changes over the other window's", p);
    return y - 36.f;
}

void WolfAdventureStudioView::editTick()
{
    const F64 t = now();
    // a field is written 0.8 s after the last key (then the 2 s autosave); test flags only on Enter
    if (mField == F_EDIT && mEditAt > 0.0 && t - mEditAt >= EDIT_IDLE_SEC && mEditDoc != 2 && mEditType != 'k') commitEdit();
    if (mPage == SP_TEST || (mPage == SP_WIZARD && mWizStep == W_TEST)) pollTest(false);   // [review] the wizard's Test step too
    // [WOLF STORY FIRST M3 2026-10-17] §9.1: the walking helper follows the author to a new region only when told where they are
    if (mHelperWalking && !mHelperWalkBusy && !mOpenId.empty())
        if (LLViewerRegion* rgn = gAgent.getRegion())
        {
            const std::string here = rgn->getRegionID().asString();
            if (here != mHelperWalkFailedRegion) mHelperWalkFailedRegion.clear();
            if (!mHelperWalkRegion.empty() && here != mHelperWalkRegion && mHelperWalkFailedRegion.empty()) helperWalk(true);
        }
    // [review] the walking state read again now and then: the brain removes a helper nobody used for 30 minutes
    if (mPage == SP_MAP && mHelperWalking && !mOpenId.empty() && t >= mHelperWalkCheckAt)
    {
        mHelperWalkCheckAt = t + 60.0;
        mHelperLoaded = false;
        helperHistory();
    }
    if (mLookReloadAt > 0.0 && t >= mLookReloadAt) lookReload();
    // [IN-WORLD STUFF 2026-10-17] a pad clicked in the world, a thing being made, and whether the brain has Clean up the land
    padsTick();
    meshTick();
    landCleanupTick();
    if (mFocusScroll && t - mFocusAt > 1.5) mFocusScroll = false;   // a problem whose field is not drawn as one
    if (!mDraftJson.empty()) loadPreset();
    // the place tool belongs to the Places page only; while it is out, its nudges are written after a pause
    if (!placesPageShown()) releasePlaceTool();
    else if (WolfToolAdventurePlace::instanceExists()) WolfToolAdventurePlace::getInstance()->tick();
}

void WolfAdventureStudioView::releasePlaceTool()
{
    WolfToolAdventurePlace* tool = WolfToolAdventurePlace::getInstance();
    if (!tool->attached()) return;
    tool->detach();
    LLToolMgr* tm = LLToolMgr::getInstance();
    if (tm->getCurrentTool() == tool) tm->clearTransientTool();
}

bool WolfAdventureStudioView::editPress(S32 id, S32 arg)
{
    switch (id)
    {
    case SH_E:
    {
        if (arg < 0 || arg >= (S32)mActs.size()) return true;
        const std::function<void()> fn = mActs[arg].mFn;   // a copy: the call may rebuild nothing, but never trust it
        if (fn) fn();
        return true;
    }
    case SH_E_TEXT:
        startEdit(arg);
        return true;
    case SH_PROBLEM:
        if (mProblems.isArray() && arg >= 0 && arg < (S32)mProblems.size()) gotoProblem(mProblems[arg]["path"].asString());
        return true;
    // [WOLF STORY FIRST 2026-10-17] a flowchart box clicked (or Enter on it): the helper's box; an ending opens its words
    case SH_FLOW_BOX:
        if (arg >= 0 && arg < (S32)mFlowNodes.size())
        {
            // [GAME CARD 2026-10-18] an ending opens its small panel under the map (win / lose / neither, and its full editor)
            if (mFlowNodes[arg].mEnding) { mFlowSelEnd = mFlowNodes[arg].mId; mFlowSel.clear(); return true; }
            mFlowSelEnd.clear();
            mFlowSel = mFlowNodes[arg].mId;
            helperFocus(mFlowSel);   // [M3] what the author says to the helper in the world is about this box
            mField = F_HELPER;   // ready to say what happens here
            mHits.setFocus(SH_FIELD, F_HELPER);
        }
        return true;
    case SH_FLOW_BG:
    case SH_FLOW_HANDLE:
        return true;
    default:
        return false;
    }
}

bool WolfAdventureStudioView::editKey(KEY key, MASK mask)
{
    if (mPicker.mOn)
    {
        // the list is modal: every key is its own
        const std::vector<S32> rows = pickerRows();
        auto step = [&](S32 by)
        {
            // move by `by` rows, past the group headings, never off either end
            if (rows.empty()) return;
            const S32 last = (S32)rows.size() - 1;
            const S32 dir = by > 0 ? 1 : -1;
            S32 k = llclamp(mPickerSel + by, 0, last);
            while (k > 0 && k < last && mPicker.mOpts[rows[k]].first == "~hdr") k += dir;
            if (mPicker.mOpts[rows[k]].first == "~hdr") k = mPickerSel;
            mPickerSel = k;
            mPickerSelMoved = true;
            mKeyboardNav = true;
        };
        switch (key)
        {
        case KEY_ESCAPE: closePicker(); break;
        case KEY_DOWN: step(1); break;
        case KEY_UP: step(-1); break;
        case KEY_TAB: step(mask & MASK_SHIFT ? -1 : 1); break;
        case KEY_PAGE_DOWN: step(10); break;
        case KEY_PAGE_UP: step(-10); break;
        case KEY_RETURN:
            if (!rows.empty()) pickerChoose(rows[llclamp(mPickerSel, 0, (S32)rows.size() - 1)]);
            break;
        case KEY_BACKSPACE:
            if (!mPickerFilter.empty())
            {
                LLWString w = utf8str_to_wstring(mPickerFilter);
                w.pop_back();
                mPickerFilter = wstring_to_utf8str(w);
                mPickerSel = 0;
                mPickerScroll = 0.f;
            }
            break;
        default: break;
        }
        return true;
    }
    // a shape being placed takes its keys from this window too (STUDIO §6 2.1.1: Enter, Escape, Backspace, the
    // arrows and [ ]), so clicking a Studio button mid-shape does not strand the keyboard
    if (mField != F_EDIT && WolfToolAdventurePlace::instanceExists())
    {
        WolfToolAdventurePlace* tool = WolfToolAdventurePlace::getInstance();
        if (tool->attached() && tool->mode() != WolfToolAdventurePlace::M_NONE && tool->handleKey(key, mask)) return true;
    }
    if (mField != F_EDIT) return false;
    const bool multi = mEditType == 'j' || (mEditType == 's' && mEditCap > 240);
    if (key == KEY_BACKSPACE && mask == MASK_NONE)
    {
        LLWString w = utf8str_to_wstring(mEditBuf);
        if (!w.empty()) w.pop_back();
        mEditBuf = wstring_to_utf8str(w);
        mEditAt = now();
        return true;
    }
    if (key == KEY_RETURN && mask == MASK_SHIFT && multi)
    {
        if (mEditCap == 0 || utf8str_to_wstring(mEditBuf).size() < mEditCap) mEditBuf += "\n";
        mEditAt = now();
        return true;
    }
    if (key == KEY_RETURN && mask == MASK_NONE)
    {
        commitEdit(true);
        return true;
    }
    if (key == KEY_ESCAPE && mask == MASK_NONE)
    {
        // Escape leaves the field. What was typed in the last 0.8 s (not yet written: EDIT_IDLE_SEC) is dropped;
        // anything already written stays (undo is not part of P3)
        mField = F_NONE;
        mEditAt = 0.0;
        if (mEditErrorPtr == mEditPtr) { mEditError.clear(); mEditErrorPtr.clear(); }
        return true;
    }
    if (key == 'V' && (mask & MASK_CONTROL))
    {
        LLWString pasted;
        if (LLClipboard::instance().pasteFromClipboard(pasted))
        {
            // control characters never go into the pack: tabs become spaces, line breaks stay only in a
            // multi-line field (as Shift+Enter types them), everything else below U+0020 and DEL is dropped
            LLWString w = utf8str_to_wstring(mEditBuf);
            for (llwchar c : pasted)
            {
                if (c == '\t') c = ' ';
                if (c == '\r') continue;
                if (c == '\n') { if (multi) w.push_back(c); else w.push_back(' '); continue; }
                if (c < 0x20 || c == 0x7f) continue;
                w.push_back(c);
            }
            const size_t lim = mEditCap ? mEditCap : 20000;
            if (w.size() > lim) w.resize(lim);
            mEditBuf = wstring_to_utf8str(w);
            mEditAt = now();
        }
        return true;
    }
    return false;
}

bool WolfAdventureStudioView::editChar(llwchar c)
{
    if (mPicker.mOn)
    {
        if (c >= 0x20 && c != 0x7f && utf8str_to_wstring(mPickerFilter).size() < 40)
        {
            LLWString w = utf8str_to_wstring(mPickerFilter);
            w.push_back(c);
            mPickerFilter = wstring_to_utf8str(w);
            mPickerSel = 0;
            mPickerScroll = 0.f;
        }
        return true;
    }
    if (mField != F_EDIT) return false;
    if (c < 0x20 || c == 0x7f) return true;
    LLWString w = utf8str_to_wstring(mEditBuf);
    const size_t lim = mEditCap ? mEditCap : 20000;
    if (w.size() < lim) w.push_back(c);
    mEditBuf = wstring_to_utf8str(w);
    mEditAt = now();
    return true;
}

bool WolfAdventureStudioView::editEscape()
{
    if (mPicker.mOn) { closePicker(); return true; }
    WolfToolAdventurePlace* tool = WolfToolAdventurePlace::getInstance();
    if (tool->attached() && tool->mode() != WolfToolAdventurePlace::M_NONE)
    {
        tool->cancel();
        setStatus("Stopped placing - nothing was changed.", false);
        return true;
    }
    return false;
}

bool WolfAdventureStudioView::editWheel(S32 clicks)
{
    if (!mPicker.mOn) return false;
    mPickerScroll = llmax(0.f, mPickerScroll + (F32)clicks * PICK_ROW);
    return true;
}

bool WolfAdventureStudioView::editConfirmText(S32 what, std::string& title, std::string& body, std::string& yes, bool& danger) const
{
    switch (what)
    {
    case SC_EDIT_DELETE:
        // [SIMPLE STUDIO 2026-10-17] page 2's Remove (askDelete remove = true): "Remove <name>?" / Remove / Keep (drawConfirm)
        title = (mConfirmKeyB == "remove" ? "Remove " : "Delete ") + mConfirmKeyA + "?";
        body = "They are taken out of your game. Anything that still refers to them shows as a problem until you change it.";
        yes = mConfirmKeyB == "remove" ? "Remove" : "Delete";
        danger = true;
        return true;
    case SC_TEST_TELEPORT:
        title = "Teleport to " + mConfirmKeyA + "?";
        body = "The test's scene is on " + mConfirmKeyA + ". Its stage is set there; the debug overlay keeps working wherever you are.";
        yes = "Teleport";
        return true;
    case SC_PLACE_TELEPORT:
        title = "Teleport to " + mConfirmKeyA + "?";
        body = "Places are put in the world on their own region. You arrive beside it; the Studio stays open.";
        yes = "Teleport";
        return true;
    case SC_RECOVER:
        title = "Restore your unsaved changes?";
        body = "This adventure has changes from your last visit that may not have been saved (the Studio was closed or the viewer quit "
               "while they were waiting, or another window saved over them). Restore puts them back as the draft; Discard keeps the draft as it is.";
        yes = "Restore";
        return true;
    case SC_PREMIUM:
        title = "Use a premium voice?";
        body = mConfirmKeyA + " is an ElevenLabs voice: every line spoken costs " + mConfirmKeyB + " credits per 100 characters "
               "from your credits. The free voices cost nothing. You are asked once in this Studio.";
        yes = "Use it";
        return true;
    case SC_COND_REPLACE:
        title = "Replace this group?";
        body = "It holds " + mConfirmKeyA + " rule(s); the new kind of rule takes its place and they are taken out.";
        yes = "Replace";
        danger = true;
        return true;
    case SC_PRESET:
        title = "Use the " + mConfirmKeyA + " rules?";
        body = mConfirmKeyB.empty()
            ? std::string("Every stat and check this pack uses is in ") + mConfirmKeyA + ". Your changes to the rules are kept."
            : mConfirmKeyA + " does not have these, which the pack uses: " + mConfirmKeyB + ". They show as problems until you change them. Your changes to the rules are kept.";
        yes = "Use " + mConfirmKeyA;
        return true;
    case SC_MESH_CANCEL:
        // [IN-WORLD STUFF 2026-10-17] Stop on a thing being made (the build's own words, wolffloateradventurestudio.cpp SC_JOB_CANCEL)
        title = "Stop making " + mMeshName + "?";
        body = "What has been spent so far is not given back.";
        yes = "Stop";
        danger = true;
        return true;
    case SC_LAND_CLEANUP:
        // [IN-WORLD STUFF 2026-10-17] Paul's words; the game's name typed into the field (drawConfirm typedConfirm)
        title = "Clean up the land?";
        body = "This takes every character, look pad and object the game put on " + mConfirmKeyA + " off the land. Your own objects stay. "
               "Type the game's name to confirm:";
        yes = "Clean up";
        danger = true;
        return true;
    default:
        return false;
    }
}

namespace
{
    // Source: wolffloaterairports.cpp:239 - the app/teleport SLURL the chat bar's own teleport command dispatches
    void teleportTo(const std::string& region, S32 x, S32 y, S32 z)
    {
        LLURLDispatcher::dispatch(llformat("secondlife:///app/teleport/%s/%d/%d/%d", LLWeb::escapeURL(region).c_str(), x, y, z), "clicked", NULL, true);
    }
}

void WolfAdventureStudioView::editAnswer(S32 what)
{
    switch (what)
    {
    case SC_EDIT_DELETE:
    {
        const std::string ptr = mDeletePtr;
        // [review] a beat goes the wizard's way: what led to it then leads where it led (no dangling next, no dead end)
        const std::vector<std::string> dt = ptrTokens(ptr);
        if (mDeleteDoc == 0 && dt.size() == 6 && dt[0] == "acts" && dt[2] == "scenes" && dt[4] == "beats") removeStoryBeat(ptr);
        else docErase(mDeleteDoc, ptr);
        for (auto it = mOpen.begin(); it != mOpen.end();) it = (*it == ptr || it->rfind(ptr + "/", 0) == 0) ? mOpen.erase(it) : std::next(it);
        if (mSel == ptr || mSel.rfind(ptr + "/", 0) == 0) mSel.clear();
        if (ptr.rfind("/cast/", 0) == 0) mCastSel2 = llmax(0, mCastSel2 - 1);
        break;
    }
    case SC_TEST_TELEPORT:
        teleportTo(mConfirmKeyA, 128, 128, 0);
        break;
    case SC_PLACE_TELEPORT:
    {
        F32 x = 128.f, y = 128.f, z = 0.f;
        sscanf(mConfirmKeyB.c_str(), "%f,%f,%f", &x, &y, &z);
        teleportTo(mConfirmKeyA, ll_round(x), ll_round(y), ll_round(z));
        break;
    }
    case SC_RECOVER:
    {
        const std::string text = mRecoveryText;
        mRecoveryText.clear();
        LLFile::remove(recoveryPath(mOpenId));
        boost::system::error_code ec;
        const boost::json::value v = boost::json::parse(text, ec);
        if (ec || !v.is_object()) { setStatus("The saved changes could not be read.", true); break; }
        // the restored draft is saved over the current one (with its etag) by the usual autosave
        if (editDoc([&v](boost::json::value& root) { root = v; return true; }))
        {
            repairLegacyDraft();   // [WOLF ADVENTURE AUDIT 2026-10-17] what an older Studio left in them is tidied too
            setStatus("Your unsaved changes are back; they save in two seconds.", false);
        }
        mRulesGen = 0;
        break;
    }
    case SC_PREMIUM:
        mPremiumOk = true;
        pickVoice(mPremiumCast, mPremiumVoice);
        break;
    case SC_COND_REPLACE:
    {
        boost::system::error_code ec;
        const boost::json::value v = boost::json::parse(mCondNew, ec);
        if (!ec) docSet(0, mCondPtr, v);
        break;
    }
    case SC_PRESET:
        docSet(0, "/rules/preset", boost::json::value(mPresetPick));
        mRulesGen = 0;
        setStatus("The rules are now " + mConfirmKeyA + ".", false);
        break;
    case SC_MESH_CANCEL:   // [IN-WORLD STUFF 2026-10-17]
    {
        if (mMeshJobId.empty()) break;
        LLSD body;
        body["job"] = mMeshJobId;
        mMeshBusy = true;
        const std::string job = mMeshJobId;
        LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
        // wolfmind_lib.php wa_api_job_cancel: queued / running -> cancelled; the next poll says so (meshPoll)
        WolfAdventure::api("job_cancel", body, [handle, job](bool ok, const LLSD&, const std::string& error)
        {
            if (handle.isDead()) return;
            WolfAdventureStudioView* self = handle.get();
            self->mMeshBusy = false;
            if (self->mMeshJobId != job) return;
            if (!ok) { self->mMeshError = "Not stopped: " + error; return; }
            self->mMeshPollAt = now();
        });
        break;
    }
    case SC_LAND_CLEANUP: landCleanup(); break;   // [IN-WORLD STUFF 2026-10-17]
    default:
        break;
    }
}

void WolfAdventureStudioView::editDeclined(S32 what)
{
    if (what != SC_RECOVER) return;
    // Discard: the draft stays as the service has it, and the old changes are not offered again
    mRecoveryText.clear();
    LLFile::remove(recoveryPath(mOpenId));
}

bool WolfAdventureStudioView::editDrop(S32 x, S32 y, bool drop, S32 cargo_type, void* cargo_data, EAcceptance* accept, std::string& tip)
{
    if (cargo_type != DAD_TEXTURE) return false;
    std::string ptr;
    for (const auto& d : mDrops) if (d.first.pointInRect(x, y)) ptr = d.second;
    if (ptr.empty()) return false;
    *accept = ACCEPT_NO;
    const LLToolDragAndDrop::ESource src = LLToolDragAndDrop::getInstance()->getSource();
    LLInventoryItem* item = (LLInventoryItem*)cargo_data;
    if (!item || (src != LLToolDragAndDrop::SOURCE_AGENT && src != LLToolDragAndDrop::SOURCE_LIBRARY))
    {
        tip = "Drag the texture from your own inventory.";
        return true;
    }
    if (item->getAssetUUID().isNull())
    {
        // the asset id is visible to the viewer only for full-permission items (as the notecards on Import)
        tip = "The viewer cannot see this texture's id: it needs full permissions.";
        return true;
    }
    *accept = ACCEPT_YES_COPY_SINGLE;
    tip = "Use \"" + item->getName() + "\" here";
    if (drop) docSet(0, ptr, boost::json::value(item->getAssetUUID().asString()));
    return true;
}

// ── leaving an adventure: nothing typed is lost (recovery file), nothing of it keeps running ─────────────────

std::string WolfAdventureStudioView::recoveryPath(const std::string& adv) const
{
    // the adventure id is a uuid from the service (BRAIN_API_V1.md §5.1): anything else never names a file
    if (!LLUUID::validate(adv)) return std::string();
    return gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "wolfadventure_unsaved_" + adv + ".json");
}

void WolfAdventureStudioView::writeRecovery()
{
    // the draft as it stands here, when it is not known to be saved: changed and not saved, a save still on
    // its way, or a conflict not yet answered. Offered back the next time it is opened (checkRecovery).
    if (mOpenId.empty() || mDraftJson.empty() || !(mDirtyAt > 0.0 || mSaving || mConflict)) return;
    const std::string path = recoveryPath(mOpenId);
    if (path.empty()) return;
    llofstream out(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (out.is_open()) out << mDraftJson;
}

void WolfAdventureStudioView::checkRecovery()
{
    const std::string path = recoveryPath(mOpenId);
    if (path.empty() || !gDirUtilp->fileExists(path)) return;
    llifstream in(path.c_str(), std::ios::in | std::ios::binary);
    std::stringstream ss;
    if (in.is_open()) ss << in.rdbuf();
    in.close();
    const std::string text = ss.str();
    boost::system::error_code ec;
    const boost::json::value v = boost::json::parse(text, ec);
    // the same as the saved draft (the save went through after all), or unreadable: nothing to offer
    if (ec || !v.is_object() || text == mDraftJson)
    {
        LLFile::remove(path);
        return;
    }
    mRecoveryText = text;
    ask(SC_RECOVER, 0);
}

void WolfAdventureStudioView::leaveOpen(bool app_quitting)
{
    // [WOLF ADVENTURE P3 2026-10-16] before another adventure opens, or the Studio closes
    releasePlaceTool();
    closePicker();
    if (LLFloater* fp = mTexPicker.get()) fp->closeFloater();
    if (mField == F_EDIT) commitEdit();
    mField = F_NONE;
    if (!mOpenId.empty())
    {
        writeRecovery();
        if (!app_quitting)
        {
            if (mDirtyAt > 0.0 && !mSaving && !mConflict) saveDraftNow();
            // [WOLF STORY FIRST 2026-10-17] characters the helper brought to show the author leave with it (helper_end)
            if (mHelperFor == mOpenId && mHelperLog.isArray() && mHelperLog.size() > 0)
            {
                LLSD hb;
                hb["adventure"] = mOpenId;
                WolfAdventure::api("helper_end", hb, nullptr);
            }
            if (!mTestSession.empty())
            {
                // a test is the owner's alone and ends when its adventure is left (its characters leave the region)
                LLSD body;
                body["session"] = mTestSession;
                WolfAdventure::api("test_end", body, nullptr);
            }
        }
    }
    mConflict = false;
    mConflictEtag.clear();
    mDirtyAt = 0.0;
    mSaving = false;
    mTestSession.clear();
    mTestState = LLSD();
    mTestFlags = LLSD::emptyMap();
    mTestError.clear();
    mTestRegionName.clear();
    mTestPollAt = 0.0;
    mSel.clear();
    mOpen.clear();
    mEditPtr.clear();
    mEditBuf.clear();
    mEditError.clear();
    mEditErrorPtr.clear();
    mEditAt = 0.0;
    mRibbonFrom = -1;
    mRulesGen = 0;
    mFocusPtr.clear();
    mFocusScroll = false;
    mLookCast.clear();
    mLookReloadAt = 0.0;
    mGameLinksFor.clear();
    mRecoveryText.clear();
}

void WolfAdventureStudioView::editClosed(bool app_quitting)
{
    // the open adventure kept open (the Studio shows it again when reopened), its work flushed: the field written,
    // the draft saved (or, quitting, kept in the recovery file), the place tool released and any test ended
    releasePlaceTool();
    closePicker();
    if (LLFloater* fp = mTexPicker.get()) fp->closeFloater();
    if (mField == F_EDIT) commitEdit();
    mField = F_NONE;
    // [SIMPLE STUDIO 2026-10-17] closed during the cost question, or on the way to it: as pressing No - the quoted job is dropped
    // and the game this attempt made goes again (no stray card, nothing charged); from "Yes, build it" on the build carries on
    if (mBuildStep >= B_CREATING && mBuildStep < B_STARTING)
    {
        if (mBuildStep == B_QUOTED) buildNo();
        else { mBuildStep = B_NONE; mBuildError.clear(); buildUnmake(); }
    }
    writeRecovery();
    if (app_quitting) return;
    if (mDirtyAt > 0.0 && !mSaving && !mConflict) saveDraftNow();
    if (!mTestSession.empty())
    {
        LLSD body;
        body["session"] = mTestSession;
        WolfAdventure::api("test_end", body, nullptr);
        mTestSession.clear();
        mTestState = LLSD();
    }
    // [WOLF STORY FIRST 2026-10-17] the helper's brought characters leave when the Studio closes (helper_end)
    if (!mOpenId.empty() && mHelperFor == mOpenId && mHelperLog.isArray() && mHelperLog.size() > 0)
    {
        LLSD hb;
        hb["adventure"] = mOpenId;
        WolfAdventure::api("helper_end", hb, nullptr);
    }
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// [WOLF ADVENTURE AUDIT 2026-10-17] the helpers shared with the wizard and the Guide (wolfadventurestudioutil.h)
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

namespace WolfAdvStudio
{
    F32 btnW(const std::string& label, F32 min_w) { return ::btnW(label, min_w); }
    boost::json::value* jAt(boost::json::value& root, const std::string& ptr, bool create) { return ::jAt(root, ptr, create); }
    const LLSD& sdAt(const LLSD& root, const std::string& ptr) { return ::sdAt(root, ptr); }
    std::string uniqueId(const std::string& base, const std::string& pack_text) { return ::uniqueId(base, pack_text); }
    bool parseColour(const std::string& s, LLColor4& out) { return ::parseColour(s, out); }
    bool packHas(const std::string& key) { return !::prop(::packSchema(), key).is_null(); }
    F32 contrast(const LLColor4& a, const LLColor4& b)
    {
        // WCAG 2.2 relative luminance: each sRGB channel c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ^ 2.4,
        // L = 0.2126 R + 0.7152 G + 0.0722 B; contrast ratio = (L1 + 0.05) / (L2 + 0.05), L1 the lighter
        auto lin = [](F32 c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); };
        auto lum = [&lin](const LLColor4& c) { return 0.2126f * lin(c.mV[VRED]) + 0.7152f * lin(c.mV[VGREEN]) + 0.0722f * lin(c.mV[VBLUE]); };
        const F32 la = lum(a), lb = lum(b);
        return (llmax(la, lb) + 0.05f) / (llmin(la, lb) + 0.05f);
    }
    LLColor4 inkOn(const LLColor4& bg)
    {
        const LLColor4 white(1.f, 1.f, 1.f, 1.f), black(0.f, 0.f, 0.f, 1.f);
        return contrast(white, bg) >= contrast(black, bg) ? white : black;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// [IN-WORLD STUFF 2026-10-17] IN_WORLD_STUFF_PLAN.md: look pads (page 2), the things a box needs and "Make it for me"
// (page 3's box panel), "Clean up the land" (page 3). Brain contracts (the coordinator's, 2026-10-17; the PHP was not
// there yet when this was written - field names as the contract gives them):
//   look_pad {adventure, cast, here} / look_pads_clear {adventure}; owner_settings {lookPadsAnyone}
//   box_things {adventure, box} -> {things: [{id, name, why, script, done}]}
//   mesh_quote {adventure, thing, prompt} -> {job, quote{credits}}; mesh_start {job, confirm:true}; job {job} as a build's
//   land_cleanup {adventure, region, confirm} (confirm = the game's exact title)
// A brain without an action answers "unknown action" (wolfadventure_lib.php:5002): that button is then not shown.
// ═══════════════════════════════════════════════════════════════════════════════════════

namespace
{
    bool unknownAction(const std::string& error) { return error.find("unknown action") != std::string::npos; }
}

std::string WolfAdventureStudioView::lookWords(const LLSD& c)
{
    // Paul (2026-10-17): characters get a STANDARD look until they are dressed - the cast sheet carries look.standard = true until a
    // pad touch or Use what I'm wearing sets look.appearance (and the brain writes look.dressedBy = who clicked the pad)
    const LLSD& look = c["look"];
    const bool standard = look["standard"].isBoolean() && look["standard"].asBoolean();
    const std::string appearance = look["appearance"].asString(), by = WolfGame::cleanText(look["dressedBy"].asString(), 60);
    if (standard || appearance.empty()) return "Standard look until you dress them: put a look pad on the land and click it wearing their outfit.";
    if (!by.empty()) return "Dressed like " + by;
    return "Dressed in what you were wearing";
}

F32 WolfAdventureStudioView::lookPadsHead(F32 l, F32 r, F32 top, const Palette& p)
{
    // the owner setting, read with the other owner settings (owner_settings {} -> settings); shown only once the brain reports
    // lookPadsAnyone (an older brain does not: nothing is drawn, never a switch that does nothing)
    if (!mOwnerLoaded) { loadOwnerSettings(); return top; }
    if (!mOwner.isMap() || !mOwner.has("lookPadsAnyone")) return top;
    const bool on = mOwner["lookPadsAnyone"].asBoolean();
    const std::string label = "Anyone can help dress my characters";
    const S32 ia = act([this, on]() { lookPadsAnyone(!on); });
    toggle(l, top, btnW(label + ": Off") + 12.f, label, on, SH_E, ia,
           "Off: only you can click a look pad. On: anyone on the land can click one to give a character their outfit", p, !mAnyoneBusy);
    textL(LLFontGL::getFontSansSerifSmall(), "A look pad is a round pad with the character's name: whoever clicks it, wearing an outfit, gives them that look.",
          l, top - 26.f - 2.f, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, r - l);
    return top - 26.f - 2.f - (F32)LLFontGL::getFontSansSerifSmall()->getLineHeight() - 8.f;
}

void WolfAdventureStudioView::lookPad(const std::string& cid, const std::string& name)
{
    if (mOpenId.empty() || mPadBusy || !mLookCast.empty()) return;
    if (mDirtyAt > 0.0 || mSaving)
    {
        // the brain names the pad from the saved draft: unsaved changes (a new character, a new name) go first
        if (!mSaving) saveDraftNow();
        setStatus("Saving your last changes first - press Put a look pad on the land again in a moment.", false);
        return;
    }
    const LLSD here = helperHere();   // {region, pos}, exactly as helper_do sends it
    if (!here.isMap()) { mCharsError = "Stand in the world first: the pad appears at your feet."; return; }
    LLSD body;
    body["adventure"] = mOpenId;
    body["cast"] = cid;
    body["here"] = here;
    mPadBusy = true;
    mCharsError.clear();
    const std::string adv = mOpenId;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // Source: wolfadventure_studio_lib.php wa_api_look_pad (433-456) -> {queued, region, regionName, pos, note, say}; `note` says when
    // the position came from the Studio because the live map could not see the author
    WolfAdventure::api("look_pad", body, [handle, adv, name](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        self->mPadBusy = false;
        if (self->mOpenId != adv) return;
        if (!ok)
        {
            self->mCharsError = unknownAction(error) ? std::string("Look pads need the latest adventure service, which this grid does not have yet.")
                                                     : "The pad was not put down: " + error;
            return;
        }
        const std::string note = WolfGame::cleanText(reply["note"].asString(), 200);
        self->setStatus("A pad for " + name + " is at your feet. Wear the look you want and click it." + (note.empty() ? std::string() : " " + note), false);
        self->mPadsOut = true;
        self->mPadCount = llmax(1, self->mPadCount);   // at least this one until the brain's count comes back
        self->padsRefresh();
        self->mPadPollAt = now() + 8.0;
    });
}

void WolfAdventureStudioView::padsRefresh()
{
    if (mOpenId.empty()) return;
    LLSD body;
    body["adventure"] = mOpenId;
    const std::string adv = mOpenId;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // Source: GAME_CARD_SPINE_SPEC.md "Change (brain, 2026-10-18, for the viewer agent)": get_draft `pads`. Quiet: the button simply
    // keeps its last state when the read fails (nothing the author asked for depends on it)
    WolfAdventure::api("get_draft", body, [handle, adv](bool ok, const LLSD& reply, const std::string&)
    {
        if (handle.isDead() || !ok) return;
        WolfAdventureStudioView* self = handle.get();
        if (self->mOpenId != adv || !reply.has("pads")) return;
        self->mPadCount = llmax(0, reply["pads"].asInteger());
    });
}

void WolfAdventureStudioView::lookPadsClear()
{
    if (mOpenId.empty() || mPadBusy) return;
    LLSD body;
    body["adventure"] = mOpenId;
    mPadBusy = true;
    mCharsError.clear();
    const std::string adv = mOpenId;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    WolfAdventure::api("look_pads_clear", body, [handle, adv](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        self->mPadBusy = false;
        if (self->mOpenId != adv) return;
        if (!ok)
        {
            self->mCharsError = unknownAction(error) ? std::string("Look pads need the latest adventure service, which this grid does not have yet.")
                                                     : "The pads were not taken away: " + error;
            return;
        }
        self->mPadsOut = false;
        self->mPadCount = 0;
        self->padsRefresh();   // the brain's own count (pads on regions that have not answered yet still count)
        self->mPadPollAt = 0.0;
        const std::string say = WolfGame::cleanText(reply["say"].asString(), 300);
        self->setStatus(say.empty() ? std::string("The look pads are gone.") : say, false);
    });
}

void WolfAdventureStudioView::lookPadsAnyone(bool on)
{
    if (mAnyoneBusy || !mOwnerLoaded) return;
    LLSD body;
    body["lookPadsAnyone"] = on;
    mAnyoneBusy = true;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // owner_settings with a field writes it and answers with every setting (wolfmind_lib.php wa_api_owner_settings)
    WolfAdventure::api("owner_settings", body, [handle, on](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        self->mAnyoneBusy = false;
        if (!ok) { self->mCharsError = "That was not saved: " + error; return; }
        if (reply["settings"].isMap()) self->mOwner = reply["settings"];
        self->mCharsError.clear();
        self->setStatus(on ? "Saved: anyone on the land can click a look pad to dress your characters." : "Saved: only you can click a look pad.", false);
    });
}

void WolfAdventureStudioView::padsTick()
{
    // a pad clicked in the world: the brain writes the look into the saved draft (look_touch, the save_look path) - while pads are
    // out and page 2 shows, the draft is read every 8 s and the looks taken (nothing else of the author's is touched)
    if (!mPadsOut || mPage != SP_CHARS || mPadPolling || mOpenId.empty() || mDraftJson.empty() || !mLookCast.empty()) return;
    if (mPadPollAt <= 0.0 || now() < mPadPollAt) return;
    mPadPolling = true;
    LLSD body;
    body["adventure"] = mOpenId;
    const std::string adv = mOpenId;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    WolfAdventure::api("get_draft", body, [handle, adv](bool ok, const LLSD& reply, const std::string&)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        self->mPadPolling = false;
        self->mPadPollAt = now() + 8.0;
        if (self->mOpenId != adv || !ok || !reply["pack"]["cast"].isArray()) return;
        if (reply.has("pads")) self->mPadCount = llmax(0, reply["pads"].asInteger());
        // which characters' looks changed, by id
        std::vector<std::pair<S32, LLSD>> changed;   // index in the open draft, the new look
        const LLSD& mine = std::as_const(self->mDraftPack)["cast"];
        for (const LLSD& c : llsd::inArray(reply["pack"]["cast"]))
        {
            for (S32 i = 0; mine.isArray() && i < (S32)mine.size(); ++i)
            {
                if (mine[i]["id"].asString() != c["id"].asString()) continue;
                const LLSD& a = mine[i]["look"];
                const LLSD& b = c["look"];
                if (a["appearance"].asString() != b["appearance"].asString() || a["dressedBy"].asString() != b["dressedBy"].asString()
                    || (a["standard"].isBoolean() && a["standard"].asBoolean()) != (b["standard"].isBoolean() && b["standard"].asBoolean()))
                    changed.push_back({ i, b });
            }
        }
        if (changed.empty()) return;
        if (self->mDirtyAt > 0.0 || self->mSaving)
        {
            // changes made meanwhile: keep them, take the brain's etag, and write the looks into them (as lookReload does)
            self->mEtag = reply["etag"].asString();
            for (const auto& ch : changed) self->docSet(0, llformat("/cast/%d/look", ch.first), LlsdToJson(ch.second));
        }
        else
        {
            self->setDraftText(reply["pack_json"].asString(), reply["pack"]);
            self->mEtag = reply["etag"].asString();
            self->setProblems(reply["problems"]);
            self->mRulesGen = 0;
        }
        for (const auto& ch : changed)
        {
            const std::string by = WolfGame::cleanText(ch.second["dressedBy"].asString(), 60);
            const LLSD& c = std::as_const(self->mDraftPack)["cast"][ch.first];
            const std::string nm = WolfGame::cleanText(c["first"].asString() + " " + c["last"].asString(), 60);
            if (!by.empty()) self->setStatus(nm + " now looks like " + by + ".", false);
        }
    });
}

// ---- page 3: the things a box needs ----

void WolfAdventureStudioView::loadBoxThings(const std::string& beat_id)
{
    if (mOpenId.empty() || mThingsLoading || mThingsMissing) return;
    mThingsLoading = true;
    mThingsFor = beat_id;
    mThingsGen = mDraftGen;
    mThingsError.clear();
    LLSD body;
    body["adventure"] = mOpenId;
    body["box"] = beat_id;
    const std::string adv = mOpenId;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // box_things {adventure, box} -> {things: [{id, name, why, script, done}]}
    WolfAdventure::api("box_things", body, [handle, adv, beat_id](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        self->mThingsLoading = false;
        if (self->mOpenId != adv || self->mThingsFor != beat_id) return;
        if (!ok)
        {
            if (unknownAction(error)) { self->mThingsMissing = true; return; }   // an older brain: the section is not drawn
            self->mThingsError = "What this box needs could not be read: " + error;
            self->mThings = LLSD::emptyArray();
            return;
        }
        self->mThings = reply["things"].isArray() ? reply["things"] : LLSD::emptyArray();
    });
}

F32 WolfAdventureStudioView::boxThings(F32 l, F32 r, F32 top, const std::string& beat_id, const Palette& p)
{
    if (mThingsMissing || beat_id.empty()) return top;
    if ((mThingsFor != beat_id || mThingsGen != mDraftGen) && !mThingsLoading && !mMeshBusy) loadBoxThings(beat_id);
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* f = LLFontGL::getFontSansSerif();
    const F32 slh = (F32)small->getLineHeight();
    F32 y = head(l, r, top, "Things to build here", p);
    if (mThingsLoading && (mThingsFor != beat_id || !mThings.isArray())) return para(l, r, y, "Looking at what this box needs...", p.mDim, true) - 8.f;
    if (!mThingsError.empty())
    {
        y = para(l, r, y, mThingsError, DANGER_INK) - 4.f;
        const S32 it = act([this, beat_id]() { mThingsFor.clear(); loadBoxThings(beat_id); });
        btn(l, y - ROW_H, l + btnW("Try again"), y, "Try again", SH_E, it, "Ask again what this box needs", p, !mThingsLoading);
        return y - ROW_H - 8.f;
    }
    const S32 n = mThings.isArray() ? (S32)mThings.size() : 0;
    if (n == 0) return para(l, r, y, "This box needs nothing built. Place it and move on.", p.mDim, true) - 8.f;
    for (S32 i = 0; i < n; ++i)
    {
        const LLSD& t = std::as_const(mThings)[i];
        // Source: wolfadventure_things_lib.php wat_box_things (130-217): {id "<box>/<kind>:<name>", kind, name, why, how, script (""
        // for an item inside a chest), done (a chest bound in-world), doneText}
        const std::string id = t["id"].asString(), name = WolfGame::cleanText(t["name"].asString(), 60), why = WolfGame::cleanText(t["why"].asString(), 200);
        const bool done = t["done"].asBoolean();
        const std::string script = t["script"].asString();
        const std::string how = WolfGame::cleanText(t["how"].asString(), 300), doneText = WolfGame::cleanText(t["doneText"].asString(), 120);
        // the line: "<name> - <why>", ticked when an object is linked to this box already (the tick and the word, not colour alone)
        y = para(l, r, y, (done ? "\xE2\x9C\x93 " : "") + name + (why.empty() ? std::string() : " - " + why) + (done ? " (done)" : ""), done ? OK_INK : p.mText) - 2.f;
        if (!how.empty()) y = para(l + 12.f, r, y, how + (doneText.empty() ? std::string() : " " + doneText), p.mDim, true) - 4.f;
        F32 x = l + 12.f;
        {
            const S32 ic = act([this, script, name]()
            {
                if (script.empty()) { mMeshError = "No script came back for " + name + " - something went wrong on our side. Try again."; return; }
                const LLWString w = utf8str_to_wstring(script);
                LLClipboard::instance().copyToClipboard(w, 0, (S32)w.size());
                setStatus("The script for " + name + " is on the clipboard: paste it into a New Script in your object.", false);
            });
            const F32 bw = btnW("Copy the script");
            btn(x, y - ROW_H, x + bw, y, "Copy the script", SH_E, ic, "Put the whole script on the clipboard", p, !script.empty());
            x += bw + 6.f;
        }
        if (!mMeshMissing)
        {
            // "Make it for me (Meshy)": the cost is quoted first (mesh_quote), then asked inline; nothing is charged before Yes
            std::string lab = "Make it for me (Meshy)";
            if (t["credits"].isDefined() && t["credits"].asReal() > 0.0) lab += llformat(" - about %d credits", (S32)ceil(t["credits"].asReal()));
            const bool making = !mMeshJobId.empty();
            const S32 im = act([this, id, name]() { meshQuote(id, name); });
            const F32 bw = btnW(lab);
            if (x + bw > r) { x = l + 12.f; y -= ROW_H + 4.f; }
            btn(x, y - ROW_H, x + bw, y, lab, SH_E, im, making ? std::string("One thing at a time: wait until the one being made is done")
                                                             : "Meshy makes a 3D model of " + name + " and puts it in your inventory. You see the cost first.",
                p, !mMeshBusy && !making && !done);
        }
        y -= ROW_H + 6.f;
        // the inline question for this thing (mesh_quote answered)
        if (mMeshQuote.isMap() && mMeshQuote["thing"].asString() == id)
        {
            const S32 credits = mMeshQuote["credits"].asInteger();
            y = para(l + 12.f, r, y, llformat("Making %s costs about %d credits (US $%.2f). Make it?", name.c_str(), credits, (F64)credits / 100.0), p.mText) - 4.f;
            const S32 iy = act([this]() { meshStart(); });
            const S32 in_ = act([this]() { meshNo(); });
            btn(l + 12.f, y - ROW_H, l + 12.f + btnW("Yes, make it") + 12.f, y, "Yes, make it", SH_E, iy, llformat("Charges about %d credits", credits), p, !mMeshBusy);   // [WORDING 2026-10-18] page 3's one primary is the band's
            btn(l + 12.f + btnW("Yes, make it") + 20.f, y - ROW_H, l + 12.f + btnW("Yes, make it") + 20.f + btnW("No"), y, "No", SH_E, in_, "Nothing is charged", p, !mMeshBusy);
            y -= ROW_H + 8.f;
        }
        // the progress card for this thing (the build's pattern: what is happening, the brain's step, the spend, Stop)
        if (!mMeshJobId.empty() && mMeshThing == id)
        {
            const std::string state = std::as_const(mMeshJob)["state"].asString();
            const std::string pr = WolfGame::cleanText(std::as_const(mMeshJob)["progress"].asString(), 120);
            const S64 el = mMeshRunAt > 0 ? llmax((S64)0, (S64)(time(nullptr) - mMeshRunAt)) : 0;
            const std::string clock = el >= 60 ? llformat("Working for %d min %d s", (S32)(el / 60), (S32)(el % 60)) : llformat("Working for %d s", (S32)el);
            std::string spent;
            if (std::as_const(mMeshJob)["cost"].isMap()) spent = llformat("Spent so far: %.0f credits.", std::as_const(mMeshJob)["cost"]["credits"].asReal());
            const F32 lh = (F32)f->getLineHeight();
            const F32 bh = 10.f + lh + 4.f + lh + 4.f + 6.f + 8.f + lh + (spent.empty() ? 0.f : slh + 2.f) + 8.f + 30.f + 10.f;
            card(l, y - bh, r, y, p, 8.f);
            F32 cy = y - 10.f;
            textL(LLFontGL::getFontSansSerifBold(), "Making " + name + "...", l + 12.f, cy, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, r - l - 24.f); cy -= lh + 4.f;
            textL(f, "Step: " + (pr.empty() ? (state == "queued" ? std::string("Waiting to start") : std::string("Making the model...")) : pr), l + 12.f, cy, p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l - 24.f);
            cy -= lh + 4.f;
            {
                // the moving bar, as page 1's progress card
                const F32 bl = l + 12.f, br = r - 12.f;
                rect(bl, cy - 6.f, br, cy, fade(p.mDim, 0.3f));
                const F32 tt = (F32)fmod(LLFrameTimer::getTotalSeconds(), 2.4) / 2.4f;
                const F32 u = tt < 0.5f ? tt * 2.f : (1.f - tt) * 2.f;
                const F32 hw = (br - bl) * 0.22f;
                rect(bl + u * (br - bl - hw), cy - 6.f, bl + u * (br - bl - hw) + hw, cy, p.mAccent);
                cy -= 6.f + 8.f;
            }
            textL(f, clock, l + 12.f, cy, p.mDim); cy -= lh;
            if (!spent.empty()) { textL(small, spent, l + 12.f, cy, p.mDim); cy -= slh + 2.f; }
            cy -= 8.f;
            const S32 is = act([this]() { ask(SC_MESH_CANCEL, 0); });
            btn(l + 12.f, cy - 30.f, l + 12.f + btnW("Stop", 100.f), cy, "Stop", SH_E, is, "Stop making it (asks first); what has been spent so far is not given back", p,
                !mMeshBusy && (state == "queued" || state == "running"), BTN_DANGER);
            y -= bh + 8.f;
        }
    }
    y = para(l, r, y, "Build or get the object, put the script in it, then touch it once to link it to this box.", p.mDim, true) - 4.f;
    if (!mMeshDone.empty()) y = para(l, r, y, mMeshDone, OK_INK) - 4.f;
    if (!mMeshError.empty()) y = para(l, r, y, mMeshError, DANGER_INK) - 4.f;
    return y - 6.f;
}

void WolfAdventureStudioView::meshQuote(const std::string& thing_id, const std::string& name)
{
    if (mOpenId.empty() || mMeshBusy || mMeshMissing || !mMeshJobId.empty()) return;
    mMeshError.clear();
    mMeshDone.clear();
    if (mMeshQuote.isMap() && !mMeshQuote["job"].asString().empty()) meshNo(false);   // an earlier question not answered: its job is dropped
    // the prompt: the thing's name, in the style of the story (its title)
    const std::string title = WolfGame::cleanText(std::as_const(mDraftPack)["adventure"]["title"].asString().empty() ? mOpenTitle
                                                  : std::as_const(mDraftPack)["adventure"]["title"].asString(), 80);
    LLSD body;
    body["adventure"] = mOpenId;
    body["thing"] = thing_id;
    body["prompt"] = name + (title.empty() ? std::string() : " in the style of " + title);
    mMeshBusy = true;
    const std::string adv = mOpenId;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // mesh_quote {adventure, thing, prompt} -> {job, quote{credits}}: nothing is charged until mesh_start
    WolfAdventure::api("mesh_quote", body, [handle, adv, thing_id, name](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        self->mMeshBusy = false;
        if (self->mOpenId != adv) return;
        if (!ok)
        {
            if (unknownAction(error)) { self->mMeshMissing = true; self->setStatus("Make it for me needs the latest adventure service, which this grid does not have yet.", true); return; }
            self->mMeshError = "The cost could not be worked out: " + error;
            return;
        }
        const std::string job = reply["job"].asString();
        if (job.empty()) { self->mMeshError = "The cost could not be worked out: something went wrong on our side, and nothing was charged. Try again."; return; }
        LLSD q;
        q["job"] = job;
        q["credits"] = (S32)ceil(reply["quote"]["credits"].asReal());
        q["thing"] = thing_id;
        q["name"] = name;
        self->mMeshQuote = q;
    });
}

void WolfAdventureStudioView::meshStart()
{
    if (mOpenId.empty() || mMeshBusy || !mMeshQuote.isMap()) return;
    LLSD body;
    body["job"] = mMeshQuote["job"].asString();
    body["confirm"] = true;
    mMeshBusy = true;
    const std::string adv = mOpenId, thing = mMeshQuote["thing"].asString(), name = mMeshQuote["name"].asString(), job = mMeshQuote["job"].asString();
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // mesh_start {job, confirm:true}: the quoted job runs (text-to-3D, the GLB into the author's inventory)
    WolfAdventure::api("mesh_start", body, [handle, adv, thing, name, job](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        self->mMeshBusy = false;
        if (self->mOpenId != adv) return;
        self->mMeshQuote = LLSD();
        if (!ok) { self->mMeshError = "Making " + name + " was not started: " + error; return; }
        self->mMeshJobId = reply["job"].asString().empty() ? job : reply["job"].asString();
        self->mMeshThing = thing;
        self->mMeshName = name;
        self->mMeshJob = LLSD::emptyMap();
        self->mMeshJob["state"] = "queued";
        self->mMeshRunAt = time(nullptr);
        self->mMeshPollAt = now();
    });
}

void WolfAdventureStudioView::meshNo(bool say)
{
    if (!mMeshQuote.isMap()) return;
    LLSD body;
    body["job"] = mMeshQuote["job"].asString();
    mMeshQuote = LLSD();
    // the quoted job is dropped (wolfmind_lib.php wa_api_job_cancel: quoting -> cancelled); nothing was charged
    WolfAdventure::api("job_cancel", body, nullptr);
    if (say) setStatus("Not made. Nothing was charged.", false);
}

void WolfAdventureStudioView::meshTick()
{
    if (mMeshJobId.empty() || mMeshPollAt <= 0.0 || now() < mMeshPollAt) return;
    meshPoll();
}

void WolfAdventureStudioView::meshPoll()
{
    mMeshPollAt = 0.0;
    if (mMeshJobId.empty()) return;
    LLSD body;
    body["job"] = mMeshJobId;
    const std::string job = mMeshJobId;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // job {job} -> {state, progress, cost, error} (wolfmind_lib.php wm_job_out), the states a build has
    WolfAdventure::api("job", body, [handle, job](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        if (self->mMeshJobId != job) return;
        if (!ok) { self->mMeshError = "How it is going could not be read: " + error + " - trying again."; self->mMeshPollAt = now() + 9.0; return; }
        self->mMeshJob = reply;
        const std::string state = reply["state"].asString();
        const std::string name = self->mMeshName;
        if (state == "queued" || state == "running") { self->mMeshPollAt = now() + 3.0; return; }
        self->mMeshJobId.clear();
        self->mMeshThing.clear();
        if (state == "done" || state == "applied")
        {
            self->mMeshError.clear();
            // wolfmind_lib.php wm_mesh_job: the done report's cost line ("Quoted: N credits. Charged: N credits.") and, when the charge
            // could not be taken now, that line too - the author sees what was charged
            std::string cost;
            for (const LLSD& line : llsd::inArray(reply["report"]))
            {
                const std::string t = WolfGame::cleanText(line.asString(), 300);
                if (t.rfind("Quoted:", 0) == 0 || t.rfind("The charge", 0) == 0) cost += " " + t;
            }
            self->mMeshDone = name + " is in your inventory, in the Wolf Adventure folder: rez it on your land." + cost;
            self->setStatus(self->mMeshDone, false);
            self->mThingsFor.clear();   // read again: the thing may be marked done
        }
        else if (state == "cancelled") self->mMeshError = "Making " + name + " was stopped before it finished.";
        else self->mMeshError = "Making " + name + " stopped: " + (reply["error"].asString().empty() ? std::string("it did not say why.") : WolfGame::cleanText(reply["error"].asString(), 240));
    });
}

// ---- page 3: Clean up the land ----

void WolfAdventureStudioView::landCleanupTick()
{
    // asked once per game, on page 3: an empty confirm never matches a title and an empty region names no land, so the brain can
    // only refuse - "unknown action" from an older brain hides the button, any other answer shows it
    if (mPage != SP_MAP || mOpenId.empty() || !openIsOwner() || mCleanupBusy || mCleanupFor == mOpenId) return;
    mCleanupFor = mOpenId;
    mCleanupKnown = 0;
    LLSD body;
    body["adventure"] = mOpenId;
    body["region"] = "";
    body["confirm"] = "";
    const std::string adv = mOpenId;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    WolfAdventure::api("land_cleanup", body, [handle, adv](bool ok, const LLSD&, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        if (self->mCleanupFor != adv) return;
        self->mCleanupKnown = (!ok && unknownAction(error)) ? -1 : 1;
    });
}

void WolfAdventureStudioView::landCleanupAsk()
{
    if (mOpenId.empty() || mCleanupBusy) return;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) { setStatus("Stand on the land first: Clean up the land works where you are standing.", true); return; }
    // the game's exact title, as the brain stores it (the list's title; the draft's when the list has not loaded)
    const LLSD* a = listed(mOpenId);
    const std::string title = a ? (*a)["title"].asString() : std::as_const(mDraftPack)["adventure"]["title"].asString();
    if (title.empty()) { setStatus("The game's name is not known yet - try again in a moment.", true); return; }
    mDeleteTitle = title;
    mDeleteTyped.clear();
    mConfirmKeyA = rgn->getName();
    ask(SC_LAND_CLEANUP, 0);
}

void WolfAdventureStudioView::landCleanup()
{
    if (mOpenId.empty() || mCleanupBusy) return;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) { setStatus("Stand on the land first: Clean up the land works where you are standing.", true); return; }
    LLSD body;
    body["adventure"] = mOpenId;
    body["region"] = rgn->getRegionID().asString();
    body["confirm"] = mDeleteTyped;
    mDeleteTyped.clear();
    mCleanupBusy = true;
    const std::string adv = mOpenId;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // land_cleanup {adventure, region, confirm} -> what went: characters, pads, objects (the brain's counts)
    WolfAdventure::api("land_cleanup", body, [handle, adv](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        self->mCleanupBusy = false;
        if (self->mOpenId != adv) return;
        if (!ok) { self->setStatus("Not cleaned up: " + error, true); return; }
        self->mPadsOut = false;
        self->padsRefresh();   // pads on other regions stay: the brain's own count, read again
        // Source: wolfadventure_studio_lib.php wa_api_land_cleanup (600-669) -> {characters, pads, objects, removed, regions, pending, say}:
        // a region that has not answered yet is `pending` and cleans up when it next talks to the brain (its `say` says so)
        std::string words = llformat("Cleaned up: %d characters, %d pads, %d objects.", reply["characters"].asInteger(), reply["pads"].asInteger(), reply["objects"].asInteger());
        if (reply["pending"].asInteger() > 0) words += " " + WolfGame::cleanText(reply["say"].asString(), 300);
        self->setStatus(words, false);
    });
}
