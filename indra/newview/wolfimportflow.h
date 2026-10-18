/**
 * @file wolfimportflow.h
 * @brief WolfViewer: bringing a build in (block builds and obstacle courses) - the import_quote -> import_start -> job flow,
 *        shared by the Adventure Studio's Story page and the Build > Import worlds... window.
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

#ifndef WOLF_IMPORT_FLOW_H
#define WOLF_IMPORT_FLOW_H

#include <ctime>
#include <functional>
#include <memory>
#include <string>

#include "llsd.h"
#include "stdtypes.h"

/**
 * [IMPORT WORLDS 2026-10-18] One import at a time: pick a file, ask what it costs (import_quote), send it (import_start), read
 * how it is going (job) until it is done. Source: GAME_CARD_SPINE_SPEC.md section 7 and "Importer status (14:40Z)";
 * the brain side is wolfstorm/php/wolfadventure_import_lib.php (wa_api_import_quote, wa_api_import_start, wai_job_out).
 * It holds no UI: the Studio draws it on its Story page, WolfFloaterImportWorlds in its own window. Made with create()
 * (the replies hold a weak reference, so a flow that is gone is never touched).
 */
class WolfImportFlow : public std::enable_shared_from_this<WolfImportFlow>
{
public:
    /** What the person chose to bring in: the brain's kind and the file types accepted for it. The Studio's words never name
     *  a brand (GAME_CARD_SPINE_SPEC.md section 7 "Never Minecraft in names"); the Import worlds window's do (the 15:20Z change). */
    enum EChoice
    {
        C_BLOCK_ANY = 0,   // Studio: a block build (.schem, .litematic, .vox)
        C_COURSE,          // Studio: an obstacle course (.rbxlx, .rbxl)
        C_MINECRAFT,       // window: Minecraft build (.schem, .litematic)
        C_ROBLOX,          // window: Roblox place (.rbxl, .rbxlx)
        C_VOXEL,           // window: MagicaVoxel model (.vox)
    };

    static std::shared_ptr<WolfImportFlow> create();

    /** Opens the file picker; the chosen file is read and its cost asked for. Does nothing while busy or a job is running. */
    void pick(EChoice choice);
    /** "Yes, bring it in" / "Import it": sends the file (import_start). */
    void start();
    /** "No": nothing is sent or charged; the file is let go. */
    void dismissQuote();
    /** "Done": the finished (or failed) card is put away. */
    void finish();
    /** Reads the job again when it is due. Call it every frame (draw or idle) while the owner is shown. */
    void tick();
    /** "Buy credits...": the brain's buy page, opened only when it is on wolf-grid.com (else an error says so). */
    void openBuyPage();

    // what the owner shows
    const std::string& kind() const { return mKind; }                 // block | obby | "" (no file chosen)
    EChoice choice() const { return mChoice; }
    const std::string& file() const { return mFile; }                 // the file's name, no folder, at most 63 characters
    S64 bytes() const { return mBytes; }
    bool busy() const { return mBusy; }                                // a call is out
    bool hasData() const { return !mData.empty(); }                   // read and not sent yet
    /** The cost question is up: the quote is back and the file not sent yet. */
    bool asking() const { return mQuote.isMap() && !mData.empty(); }
    /** import_quote's reply ({kind, name, size, quote{credits, micro}, limits, formats, legal, studMetres?}). */
    const LLSD& quote() const { return mQuote; }
    S32 quoteCredits() const;
    bool hasJob() const { return !mJobId.empty(); }
    const std::string& jobId() const { return mJobId; }
    bool running() const;
    const LLSD& job() const { return mJob; }                          // job {job}'s reply
    std::string state() const;
    time_t runAt() const { return mRunAt; }
    const std::string& error() const { return mError; }               // red text that stays until the next file
    bool needCredits() const { return mNeedCredits && !mBuyUrl.empty(); }
    /** A done job's short green heading: "Done!", or "Done - your game is made." when the import made a game. Where the box went
     *  and how to rez it are the brain's own report lines (the coordinator, 2026-10-18: no sentence twice). */
    static std::string doneWords(const LLSD& job);
    /** The game a done obstacle-course import made or set up (output.adventure), or "". */
    std::string madeAdventure() const;

    /** [IMPORT OWNERS 2026-10-18] Paul: "only region owners can import stuff and only on our grid". False once the brain has
     *  refused this person with code not_region_owner (403); the choices are then greyed out with notAllowedWhy() shown. */
    bool importAllowed() const { return !mNotOwner; }
    const std::string& notAllowedWhy() const { return mNotOwnerWhy; }
    /** Asks the brain whether this person may import (import_allowed {}), so the choices are greyed out before they try. A brain
     *  without the action, or a failed call, leaves things as they are (import_quote refuses anyway). */
    void checkAllowed();
    /** Where to rent a region (the refusal's shopUrl, https only; "" when the grid names none). */
    const std::string& shopUrl() const { return mShopUrl; }
    void openShopPage();

    bool makeGame() const { return mMakeGame; }
    void setMakeGame(bool on) { mMakeGame = on; }

    /** Runs once each time a job finishes as done (the Studio reloads its list of games). */
    void setOnDone(std::function<void()> cb) { mOnDone = std::move(cb); }

private:
    WolfImportFlow() = default;
    void poll();
    void read(const std::string& path, EChoice choice);
    /** A refusal with code not_region_owner: remembered (with the brain's plain reason) instead of shown as a failure. */
    bool refusedNotOwner(const LLSD& reply, const std::string& error);

    EChoice mChoice = C_BLOCK_ANY;
    std::string mKind;              // block | obby
    std::string mFile, mData;       // the file's name and its bytes as base64
    S64 mBytes = 0;
    LLSD mQuote;                    // import_quote's reply (undefined: not asked / not back yet)
    bool mBusy = false;
    std::string mError;
    bool mNeedCredits = false;      // import_start refused for too few credits (code credits_short / buyurl)
    std::string mBuyUrl;
    std::string mJobId;
    LLSD mJob;
    F64 mPollAt = 0.0;
    time_t mRunAt = 0;
    S32 mPollFails = 0;
    bool mNotOwner = false;         // the brain said not_region_owner
    std::string mNotOwnerWhy;
    std::string mShopUrl;
    bool mCheckBusy = false;
    bool mMakeGame = false;         // an obstacle course: also make a new game from it (import_start game:true)
    std::function<void()> mOnDone;
};

#endif // WOLF_IMPORT_FLOW_H
