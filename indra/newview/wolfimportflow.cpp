/**
 * @file wolfimportflow.cpp
 * @brief WolfViewer: bringing a build in - shared by the Adventure Studio and the Import worlds window.
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

#include "llviewerprecompiledheaders.h"

#include "wolfimportflow.h"

#include <cmath>

#include "llbase64.h"
#include "lldir.h"
#include "llfilepicker.h"
#include "llframetimer.h"
#include "llviewermenufile.h"   // LLFilePickerReplyThread
#include "llweb.h"
#include "wolfadventure.h"
#include "wolfgame.h"

// [IMPORT WORLDS 2026-10-18] Moved here from wolffloateradventurestudioai.cpp (WolfAdventureStudioView::importPick / importStart /
// importPoll, GAME CARD 2026-10-18) so the Studio's Story page and the Import worlds window run the SAME flow. The behaviour is
// unchanged; only the wrong-file words depend on the choice (the window may name the brands, the Studio does not).

namespace
{
    // Source: GAME_CARD_SPINE_SPEC.md section 7 "Limits: 20 MB file"; wolfadventure_import_lib.php WAI_MAX_FILE (refused there too)
    const S64 IMPORT_MAX_BYTES = 20LL * 1024 * 1024;
    // Source: wolffloateradventurestudioai.cpp JOB_POLL_SEC - the Studio's job poll period, the same for an import
    const F64 JOB_POLL_SEC = 3.0;

    F64 now() { return LLFrameTimer::getTotalSeconds(); }
    std::string cap(const std::string& s, size_t n) { return WolfGame::cleanText(s, n); }

    /** Source: wolffloateradventurestudioai.cpp creditsShort (GAME_CARD_SPINE_SPEC.md "Change (brain, 2026-10-18, for the viewer
     *  agent)"): code credits_short (with buyurl) is too few credits; credits_unread has no buy link; the old text check stays. */
    bool creditsShort(const LLSD& reply, const std::string& error)
    {
        if (reply.isMap() && reply["code"].asString() == "credits_unread") return false;
        if (reply.isMap() && (reply["code"].asString() == "credits_short" || reply.has("buyurl"))) return true;
        return error.find("WolfCredits") != std::string::npos;
    }

    struct ChoiceInfo
    {
        const char* kind;          // wolfadventure_import_lib.php WAI_KINDS: block | obby
        const char* exts[3];       // lower case, no dot; WAI_FORMATS ('block' => .schem .litematic .vox, 'obby' => .rbxlx .rbxl)
        const char* wrong;         // the red line for a file of another type
    };

    const ChoiceInfo& info(WolfImportFlow::EChoice c)
    {
        static const ChoiceInfo INFO[] =
        {
            { "block", { "schem", "litematic", "vox" }, "That is not a block build file: choose a .schem, .litematic or .vox file." },
            { "obby",  { "rbxlx", "rbxl", nullptr },     "That is not an obstacle course file: choose a .rbxlx or .rbxl file (your own place file)." },
            { "block", { "schem", "litematic", nullptr }, "That is not a Minecraft build file: choose a .schem or .litematic file." },
            { "obby",  { "rbxl", "rbxlx", nullptr },     "That is not a Roblox place file: choose a .rbxl or .rbxlx file (File > Download a Copy in Roblox Studio)." },
            { "block", { "vox", nullptr, nullptr },      "That is not a MagicaVoxel model: choose a .vox file." },
        };
        return INFO[llclamp((S32)c, 0, (S32)(sizeof(INFO) / sizeof(INFO[0])) - 1)];
    }
}

// static
std::shared_ptr<WolfImportFlow> WolfImportFlow::create()
{
    return std::shared_ptr<WolfImportFlow>(new WolfImportFlow());
}

S32 WolfImportFlow::quoteCredits() const
{
    // Source: wolfadventure_import_lib.php wai_quote -> {credits, micro}; whole credits (the Studio's own rounding)
    return mQuote.isMap() ? (S32)ceil(mQuote["quote"]["credits"].asReal()) : 0;
}

bool WolfImportFlow::running() const
{
    const std::string s = state();
    return !mJobId.empty() && (s == "queued" || s == "running");
}

std::string WolfImportFlow::state() const
{
    return mJob.isMap() ? mJob["state"].asString() : std::string();
}

// static
std::string WolfImportFlow::doneWords(const LLSD& job)
{
    // Source: wolfadventure_import_lib.php wai_job_out: output.adventure = the game the course made ("" none). The report's own last
    // lines say where the box is and how to rez and clear it (wai_import_block / wai_import_obby), so they are not repeated here.
    return job["output"]["adventure"].asString().empty() ? std::string("Done!") : std::string("Done - your game is made.");
}

std::string WolfImportFlow::madeAdventure() const
{
    // Source: wolfadventure_import_lib.php wai_job_out: output {item, folder, folderName, name, pieces, adventure} when done
    return state() == "done" ? mJob["output"]["adventure"].asString() : std::string();
}

bool WolfImportFlow::refusedNotOwner(const LLSD& reply, const std::string& error)
{
    // Source: the coordinator's note (2026-10-18, Paul "only region owners can import stuff and only on our grid"): the brain refuses
    // import_quote / import_start with code not_region_owner (403) and a plain message; this sentence stands in only when it gives none
    if (!reply.isMap() || reply["code"].asString() != "not_region_owner") return false;
    mNotOwner = true;
    mNotOwnerWhy = error.empty() ? std::string("Only region owners on Wolf Territories can import builds. Rent a region to bring in your own worlds.") : error;
    const std::string shop = reply["shopUrl"].asString();
    mShopUrl = shop.rfind("https://", 0) == 0 ? shop : std::string();
    mError.clear();
    return true;
}

void WolfImportFlow::checkAllowed()
{
    if (mCheckBusy) return;
    mCheckBusy = true;
    std::weak_ptr<WolfImportFlow> weak = weak_from_this();
    // Source: wolfadventure_import_lib.php wa_api_import_allowed: import_allowed {} -> {allowed, why, code?, shopUrl?}
    // (GAME_CARD_SPINE_SPEC.md "Change (brain, imports, 2026-10-18 ~16:00Z)")
    WolfAdventure::api("import_allowed", LLSD::emptyMap(), [weak](bool ok, const LLSD& reply, const std::string& error)
    {
        std::shared_ptr<WolfImportFlow> self = weak.lock();
        if (!self) return;
        self->mCheckBusy = false;
        if (!ok || !reply.isMap() || !reply.has("allowed")) return;
        if (reply["allowed"].asBoolean())
        {
            self->mNotOwner = false;
            self->mNotOwnerWhy.clear();
            self->mShopUrl.clear();
            return;
        }
        self->refusedNotOwner(reply, reply["why"].asString());
    });
}

void WolfImportFlow::openShopPage()
{
    if (mShopUrl.rfind("https://", 0) == 0) LLWeb::loadURLExternal(mShopUrl);
}

void WolfImportFlow::pick(EChoice choice)
{
    if (mBusy || running() || mNotOwner) return;
    // the last one's card is put away once a file is chosen (read), so cancelling the picker keeps it
    std::weak_ptr<WolfImportFlow> weak = weak_from_this();
    // Source: wolffloateradventurestudioai.cpp readBookFile - the viewer's own file picker (every file shown; the extension is checked here)
    LLFilePickerReplyThread::startPicker(
        [weak, choice](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            std::shared_ptr<WolfImportFlow> self = weak.lock();
            if (!self || files.empty() || files.front().empty()) return;
            self->read(files.front(), choice);
        },
        LLFilePicker::FFLOAD_ALL, false);
}

void WolfImportFlow::read(const std::string& path, EChoice choice)
{
    if (mBusy || running()) return;   // a second picker answered while the first file is out
    std::string ext = gDirUtilp->getExtension(path);
    LLStringUtil::toLower(ext);
    mError.clear();
    mQuote = LLSD();
    mJobId.clear();
    mJob = LLSD();
    mData.clear();
    mKind.clear();
    mFile.clear();
    mNeedCredits = false;   // a Buy credits... button only for this import's own refusal
    const ChoiceInfo& ci = info(choice);
    bool okExt = false;
    for (const char* e : ci.exts) if (e && ext == e) okExt = true;
    if (!okExt) { mError = ci.wrong; return; }
    llifstream in(path.c_str(), std::ios::in | std::ios::binary);
    if (!in.is_open()) { mError = "That file could not be opened."; return; }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size <= 0) { mError = "That file is empty."; return; }
    if ((S64)size > IMPORT_MAX_BYTES) { mError = "That file is larger than 20 MB, the most that can be brought in."; return; }
    in.seekg(0, std::ios::beg);
    std::string bytes((size_t)size, '\0');
    in.read(&bytes[0], size);
    if (!in) { mError = "That file could not be read to the end."; return; }
    mChoice = choice;
    mKind = ci.kind;
    // Source: wolfadventure_import_lib.php wai_name_in: 1 to 63 characters (the extension is taken off there)
    mFile = cap(gDirUtilp->getBaseFileName(path, true), 63);   // without the extension, so a long name keeps its own end
    mBytes = (S64)size;
    mData = LLBase64::encode((const U8*)bytes.data(), bytes.size());
    // the cost first (nothing is charged for asking): import_quote {kind: block|obby, name, size} (section 7)
    LLSD body;
    body["kind"] = mKind;
    body["name"] = mFile;
    body["size"] = (LLSD::Integer)size;
    mBusy = true;
    const std::string file = mFile;
    std::weak_ptr<WolfImportFlow> weak = weak_from_this();
    WolfAdventure::api("import_quote", body, [weak, file](bool ok, const LLSD& reply, const std::string& error)
    {
        std::shared_ptr<WolfImportFlow> self = weak.lock();
        if (!self) return;
        self->mBusy = false;
        if (self->mFile != file) return;
        if (!ok)
        {
            if (!self->refusedNotOwner(reply, error)) self->mError = "It could not be brought in: " + error;
            self->mData.clear();
            self->mKind.clear();
            return;
        }
        self->mNotOwner = false;   // asked and answered: this person may import
        // Source: wolfadventure_import_lib.php wa_api_import_quote -> {kind, name, size, quote:{credits, micro}, limits, formats, legal}
        self->mQuote = reply.isMap() ? reply : LLSD::emptyMap();
    });
}

void WolfImportFlow::start()
{
    if (mBusy || mData.empty() || mKind.empty()) return;
    LLSD body;
    body["kind"] = mKind;
    body["name"] = mFile;
    body["data"] = mData;
    // Source: GAME_CARD_SPINE_SPEC.md "Change (importer agent, 2026-10-18 13:50Z)": an obstacle course also takes game:true (a NEW game
    // from the obstacle-course starter, its checkpoints as the main quest, on the region the author stands on); neither game nor
    // adventure = just the box. No open game is ever named (the brain would refuse a game not made from that starter anyway).
    if (mKind == "obby" && mMakeGame) body["game"] = true;
    mBusy = true;
    mError.clear();
    mNeedCredits = false;
    const std::string file = mFile;
    std::weak_ptr<WolfImportFlow> weak = weak_from_this();
    // Source: GAME_CARD_SPINE_SPEC.md section 7: import_start {kind, name, data: <base64 file>, adventure?} -> {job}; progress by `job`
    WolfAdventure::api("import_start", body, [weak, file](bool ok, const LLSD& reply, const std::string& error)
    {
        std::shared_ptr<WolfImportFlow> self = weak.lock();
        if (!self) return;
        self->mBusy = false;
        if (self->mFile != file) return;
        if (!ok && self->refusedNotOwner(reply, error))
        {
            self->mData.clear();
            self->mQuote = LLSD();
            self->mKind.clear();
            return;
        }
        if (!ok || reply["job"].asString().empty())
        {
            self->mError = ok ? std::string("It could not be brought in: something went wrong on our side, and nothing was charged. Try again.")
                              : "It could not be brought in: " + error;
            // Source: wolfadventure_import_lib.php wa_api_import_start - too few credits: buyurl + code credits_short
            if (creditsShort(reply, error))
            {
                self->mNeedCredits = true;
                if (!reply["buyurl"].asString().empty()) self->mBuyUrl = reply["buyurl"].asString();
            }
            return;
        }
        self->mData.clear();   // sent: not kept in memory
        self->mQuote = LLSD();
        self->mJobId = reply["job"].asString();
        self->mJob = LLSD::emptyMap();
        self->mJob["state"] = "queued";
        self->mRunAt = time(nullptr);
        self->mPollFails = 0;
        self->mPollAt = now() + JOB_POLL_SEC;
    });
}

void WolfImportFlow::dismissQuote()
{
    if (mBusy) return;
    mData.clear();
    mQuote = LLSD();
    mKind.clear();
    mFile.clear();
}

void WolfImportFlow::finish()
{
    if (running()) return;
    mJobId.clear();
    mJob = LLSD();
    mFile.clear();
    mKind.clear();
    mPollAt = 0.0;
}

void WolfImportFlow::tick()
{
    if (mPollAt > 0.0 && now() >= mPollAt) poll();
}

void WolfImportFlow::poll()
{
    mPollAt = 0.0;
    if (mJobId.empty()) return;
    LLSD body;
    body["job"] = mJobId;
    const std::string id = mJobId;
    std::weak_ptr<WolfImportFlow> weak = weak_from_this();
    // §5.5 job {job} -> {state, progress, error, output?, report?} (wolfmind_lib.php wm_job_out; wai_job_out for kind import)
    WolfAdventure::api("job", body, [weak, id](bool ok, const LLSD& reply, const std::string& error)
    {
        std::shared_ptr<WolfImportFlow> self = weak.lock();
        if (!self || self->mJobId != id) return;
        if (!ok)
        {
            // tried a few times, then said once and left (it carries on in the brain; the result lands in the inventory anyway)
            if (++self->mPollFails >= 5)
            {
                self->mError = "How the import is going could not be read (" + error + "). If it finishes, it is in your inventory under Objects > Wolf Adventure.";
                self->mJobId.clear();
                self->mJob = LLSD();
                return;
            }
            self->mError = "How the import is going could not be read: " + error + " - trying again.";
            self->mPollAt = now() + JOB_POLL_SEC * 3.0;
            return;
        }
        self->mPollFails = 0;
        self->mError.clear();
        self->mJob = reply.isMap() ? reply : LLSD::emptyMap();
        const std::string state = self->mJob["state"].asString();
        if (state == "queued" || state == "running") { self->mPollAt = now() + JOB_POLL_SEC; return; }
        if (state == "failed") self->mError = "It could not be brought in: " + cap(reply["error"].asString(), 240);
        else if (state == "cancelled") self->mError = "The import was stopped before it finished.";
        // done: the card says where it went (output) and what was left out (report); the owner may reload what it shows
        if (state == "done" && self->mOnDone) self->mOnDone();
    });
}

void WolfImportFlow::openBuyPage()
{
    // Source: wolffloateradventurestudioflow.cpp importSection's Buy credits... - only a wolf-grid.com page is opened
    if (mBuyUrl.rfind("https://www.wolf-grid.com/", 0) == 0 || mBuyUrl.rfind("https://wolf-grid.com/", 0) == 0) LLWeb::loadURLExternal(mBuyUrl);
    else mError = "The credits page address did not come from Wolf Territories, so it was not opened.";
}
