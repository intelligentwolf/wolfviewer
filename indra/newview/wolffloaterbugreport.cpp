/**
 * @file wolffloaterbugreport.cpp
 * @brief WolfViewer: Report a Problem. See wolffloaterbugreport.h.
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

#include "wolffloaterbugreport.h"

#include <ctime>

#include <boost/json.hpp>

#include "llagent.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "lldiriterator.h"
#include "llfile.h"
#include "indra_constants.h"   // APP_NAME
#include "llgl.h"
#include "llimagejpeg.h"   // [FLEA PICTURES 2026-10-27/] pictures are sent as PNG, or JPEG when the PNG is too big
#include "llimagepng.h"
#include "lllineeditor.h"
#include "llradiogroup.h"   // [FLEA REPORT 2026-10-26/] "Something's broken" / "I have an idea"
#include "llsdjson.h"
#include "llsys.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "llversioninfo.h"
#include "llviewercontrol.h"   // gSavedSettings
#include "llviewermenufile.h"  // [FLEA PICTURES 2026-10-27/] LLFilePickerReplyThread
#include "llviewerregion.h"
#include "llviewerwindow.h"    // [FLEA PICTURES 2026-10-27/] gViewerWindow->rawSnapshot
#include "llweb.h"
#include "workqueue.h"
#include "wolfgrid.h"
#include "wolflogscrub.h"   // [CRASH REPORTS 2026-10-28/] the scrub / tail / gzip code that used to live in this file

namespace
{
    // gridmanager/viewerbugreport.php on the grid website; the ticket pages are gridmanager/tickets.php
    // (index.php?f=tk, and &ticket=<id> for one ticket - ticketlib.php tkTicketUrl).
    const char* REPORT_API = "https://www.wolf-grid.com/viewerbugreport.php";
    const char* TICKETS_URL = "https://www.wolf-grid.com/index.php?f=tk";

    // The keep-the-tail cap for each log before it is compressed. viewerbugreport.php accepts up to
    // TK_LOG_MAX_FILE_BYTES (4 MB) per compressed file and 16 MB once opened (TK_LOG_MAX_RAW_BYTES); 4 MB of
    // text compresses to well under 1 MB, so four of them stay inside its 8 MB total.
    const S64 MAX_LOG_BYTES = 4 * 1024 * 1024;
    // A crash older than this is not "the problem you are reporting".
    const time_t CRASH_MAX_AGE = 7 * 24 * 60 * 60;

    // [CRASH REPORTS 2026-10-28] Moved, unchanged, to wolflogscrub.cpp so the automatic crash report shares them.
    using WolfLogScrub::size_text;
    using WolfLogScrub::wolf_scrub_log;
    using WolfLogScrub::read_tail;
    using WolfLogScrub::gzip_string;

    // ticketlib.php tkCategories(), in the website's order: the same keys, so the ticket lands in the same queue
    // filter as one raised on the website.
    const char* const CATEGORIES[][2] = {
        { "region", "Region / sim problem" },
        { "land", "Land, rentals & billing" },
        { "account", "Account & login" },
        { "payment", "Payments & currency" },
        { "marketplace", "Marketplace" },
        { "inventory", "Inventory & assets" },
        { "scripting", "Scripting problem" },
        { "teleport", "Teleporting & map problem" },
        { "abuse", "Abuse report" },
        { "other", "Something else" },
    };
    // [FLEA REPORT 2026-10-26] Paul: a flea report is also for new features and ideas. "I have an idea" sends this
    // ticketlib.php tkCategories() key (label "Idea / feature request"); it is not in the "Something's broken" list.
    const char* const IDEA_CATEGORY[2] = { "idea", "Idea / feature request" };

    bool is_idea(const LLFloater* f)
    {
        return f->getChild<LLRadioGroup>("kind")->getSelectedIndex() == 1;
    }

    std::string date_text(time_t t)
    {
        struct tm g;
#if LL_WINDOWS
        localtime_s(&g, &t);
#else
        localtime_r(&t, &g);
#endif
        char buf[32];
        strftime(buf, sizeof(buf), "%d %b %H:%M", &g);
        return buf;
    }

    // Runs on the "General" work queue: read, scrub and compress every log. Returns [file name, gzip bytes] pairs.
    std::vector<std::pair<std::string, std::string>> prepare_logs(const std::vector<WolfFloaterBugReport::LogFile>& logs,
                                                                  const std::vector<std::string>& secrets)
    {
        std::vector<std::pair<std::string, std::string>> parts;
        for (const WolfFloaterBugReport::LogFile& f : logs)
        {
            std::string text, gz;
            if (!read_tail(f.mPath, text, MAX_LOG_BYTES)) continue;
            if (!gzip_string(wolf_scrub_log(text, secrets), gz)) continue;
            parts.emplace_back(f.mSendAs + ".gz", gz);
        }
        return parts;
    }

    std::string json_string(const LLSD& v, const char* key)
    {
        return v.isMap() && v.has(key) ? v[key].asString() : std::string();
    }

    // [FLEA PICTURES 2026-10-27] Pictures. viewerbugreport.php refuses one over VB_MAX_PICTURE_BYTES (2 MB) or
    // VB_MAX_PICTURE_SIDE (4096 px) and anything that is not PNG or JPEG; the viewer keeps each to PICTURE_MAX_SIDE,
    // which keeps a screenshot readable and well under the cap.
    const S32 PICTURE_MAX_SIDE = 1920;
    const S32 PICTURE_MAX_BYTES = 2 * 1024 * 1024;
    // A picked file larger than this is not even opened (a phone photo is a few MB).
    const S64 PICTURE_MAX_FILE_BYTES = 64 * 1024 * 1024;

    // What prepare_picture hands back to the main thread: the encoded picture, or a sentence saying why not.
    struct PreparedPicture
    {
        std::string mData;
        std::string mExt;
        std::string mError;
        S32 mWidth = 0;
        S32 mHeight = 0;
    };

    std::string time_text(time_t t)
    {
        struct tm g;
#if LL_WINDOWS
        localtime_s(&g, &t);
#else
        localtime_r(&t, &g);
#endif
        char buf[16];
        strftime(buf, sizeof(buf), "%H:%M", &g);
        return buf;
    }

    // Shrink so the longest side is at most max_side, keeping the shape. Integer maths (no float to S32 conversion).
    bool fit_picture(LLImageRaw* raw, S32 max_side)
    {
        const S32 w = raw->getWidth();
        const S32 h = raw->getHeight();
        const S32 longest = llmax(w, h);
        if (longest <= max_side) return true;
        const S32 nw = llmax(1, (S32)((S64)w * max_side / longest));
        const S32 nh = llmax(1, (S32)((S64)h * max_side / longest));
        return raw->scale(nw, nh);   // llimage.cpp LLImageRaw::scale: 1, 3 or 4 components
    }

    // Encode raw with img; keep the result in out when it fits under PICTURE_MAX_BYTES.
    bool encode_into(LLImageFormatted* img, const LLImageRaw* raw, const char* ext, PreparedPicture& out)
    {
        // llimagepng.cpp / llimagejpeg.cpp encode: the bytes are getData(), getDataSize() long (allocateData).
        if (!img->encode(raw, 0.f) || !img->getData() || img->getDataSize() <= 0 || img->getDataSize() > PICTURE_MAX_BYTES)
        {
            return false;
        }
        out.mData.assign(reinterpret_cast<const char*>(img->getData()), (size_t)img->getDataSize());
        out.mExt = ext;
        out.mWidth = raw->getWidth();
        out.mHeight = raw->getHeight();
        return true;
    }

    // Runs on the "General" work queue. Re-encoding every picture also drops whatever a picked file carried besides
    // its pixels (a phone photo's location, for one): only pixels are sent.
    PreparedPicture encode_picture(LLPointer<LLImageRaw> raw)
    {
        PreparedPicture out;
        if (raw.isNull() || !raw->getData() || raw->getWidth() < 1 || raw->getHeight() < 1)
        {
            out.mError = "the picture came out empty.";
            return out;
        }
        const S8 comps = raw->getComponents();
        if ((comps != 1 && comps != 3 && comps != 4) || !fit_picture(raw, PICTURE_MAX_SIDE))
        {
            out.mError = "the picture uses a colour format the viewer cannot send.";
            return out;
        }
        // PNG first: sharp text, which a screenshot of a problem usually needs.
        LLPointer<LLImagePNG> png = new LLImagePNG;
        if (encode_into(png, raw, "png", out)) return out;

        // Too big as a PNG: JPEG, which takes 1 or 3 components only (llimagejpeg.cpp LLImageJPEG::encode), so a
        // picture with transparency loses it.
        LLPointer<LLImageRaw> rgb = raw;
        if (comps == 4)
        {
            rgb = new LLImageRaw(raw->getWidth(), raw->getHeight(), 3);
            rgb->copyUnscaled4onto3(raw);   // llimage.h: same size, 4 components onto 3
        }
        for (S32 step = 0; step < 4; ++step)
        {
            for (S32 quality : { 85, 70 })
            {
                LLPointer<LLImageJPEG> jpg = new LLImageJPEG(quality);
                if (encode_into(jpg, rgb, "jpg", out)) return out;
            }
            // Still over 2 MB: three quarters of the size, and again.
            if (!rgb->scale(llmax(1, rgb->getWidth() * 3 / 4), llmax(1, rgb->getHeight() * 3 / 4))) break;
        }
        out.mError = "the picture is too detailed to fit in " + size_text(PICTURE_MAX_BYTES) + ", even made smaller.";
        return out;
    }

    // Runs on the "General" work queue: open a picked file (already checked for its type and size) and encode it.
    PreparedPicture load_picture_file(const std::string& path, S8 codec)
    {
        PreparedPicture out;
        // As LLViewerTextureList::createUploadFile (llviewertexturelist.cpp) opens a picked image: createFromType,
        // load, decode into a raw image.
        LLPointer<LLImageFormatted> img = LLImageFormatted::createFromType(codec);
        if (img.isNull() || !img->load(path))
        {
            out.mError = "the file could not be opened.";
            return out;
        }
        LLPointer<LLImageRaw> raw = new LLImageRaw;
        if (!img->decode(raw, 0.f))
        {
            out.mError = "the file could not be read as a picture. It may be damaged, or not really a picture.";
            return out;
        }
        return encode_picture(raw);
    }
}

WolfFloaterBugReport::WolfFloaterBugReport(const LLSD& key) : LLFloater(key)
{
}

bool WolfFloaterBugReport::postBuild()
{
    mCategory = getChild<LLComboBox>("category");
    mSubject = getChild<LLLineEditor>("subject");
    mBody = getChild<LLTextEditor>("body");
    mAttach = getChild<LLCheckBoxCtrl>("attach_logs");
    mLogsList = getChild<LLTextBox>("logs_list");
    mStatus = getChild<LLTextEditor>("status");
    mSend = getChild<LLButton>("send");
    mOpenTicket = getChild<LLButton>("open_ticket");

    // [FLEA REPORT 2026-10-26] The choice at the top sets up the form for a problem or an idea. An idea has its own
    // category (IDEA_CATEGORY) and its logs are off by default (the box can still be ticked); a problem keeps them on.
    // Switching kind empties nothing the resident typed.
    auto apply_kind = [this](bool idea, bool refresh_logs)
    {
        mCategory->clearRows();
        if (idea)
        {
            mCategory->add(IDEA_CATEGORY[1], LLSD(std::string(IDEA_CATEGORY[0])));
        }
        else
        {
            mCategory->add("Choose one...", LLSD(std::string()));
            for (const auto& c : CATEGORIES)
            {
                mCategory->add(c[1], LLSD(std::string(c[0])));
            }
        }
        mCategory->selectFirstItem();
        mCategory->setEnabled(!idea);
        getChild<LLTextBox>("body_label")->setText(idea ? std::string("What is your idea?") : std::string("What happened?"));
        mBody->setLabel(idea ? std::string("What you would like, and how you would use it.")
                             : std::string("What you were doing, what you expected, and what happened instead."));
        mSubject->setLabel(idea ? std::string("For example: let me save my favourite camera views")
                                : std::string("For example: the viewer froze when I teleported"));
        mAttach->set(!idea);
        if (refresh_logs) findLogs();
    };
    LLRadioGroup* kind = getChild<LLRadioGroup>("kind");
    kind->setSelectedIndex(0);
    kind->setCommitCallback([apply_kind](LLUICtrl* ctrl, const LLSD&)
    {
        apply_kind(static_cast<LLRadioGroup*>(ctrl)->getSelectedIndex() == 1, true);
    });
    apply_kind(false, false);   // onOpen lists the logs

    mSend->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSend(); });
    getChild<LLButton>("my_tickets")->setCommitCallback([](LLUICtrl*, const LLSD&) { LLWeb::loadURLExternal(TICKETS_URL); });
    mOpenTicket->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        if (!mTicketUrl.empty()) LLWeb::loadURLExternal(mTicketUrl);
    });
    mAttach->setCommitCallback([this](LLUICtrl*, const LLSD&) { findLogs(); });

    // [FLEA PICTURES 2026-10-27]
    getChild<LLButton>("take_snapshot")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onTakeSnapshot(); });
    getChild<LLButton>("add_picture")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAddPicture(); });
    for (S32 i = 0; i < MAX_PICTURES; ++i)
    {
        getChild<LLButton>(llformat("remove_picture_%d", i + 1))->setCommitCallback([this, i](LLUICtrl*, const LLSD&)
        {
            if (mSending || i >= (S32)mPictures.size()) return;
            const std::string label = mPictures[i].mLabel;
            mPictures.erase(mPictures.begin() + i);
            updatePictures();
            setStatus("Removed: " + label + ".", EStatus::INFO);
        });
    }
    updatePictures();
    return true;
}

void WolfFloaterBugReport::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    findLogs();
    if (!mSending && !WolfGrid::isWolfTerritories())   // Paul 10-24: "only wolf residents" (a Wolf login, wherever they are)
    {
        setStatus("Flea reports are for Wolf Territories residents. To report a problem on another grid, "   // [FLEA REPORT 2026-10-26/]
                  "use that grid's own support.", EStatus::ERR);
    }
}

// Exactly which files would be sent, with their sizes, so the resident knows what they are handing over.
void WolfFloaterBugReport::findLogs()
{
    mLogs.clear();
    // llappviewer.cpp initLoggingAndGetLastDuration: this session logs to APP_NAME + ".log"; the session before is
    // renamed to APP_NAME + ".old" at start-up.
    const struct { const char* ext; const char* what; } sessions[] = { { ".log", "this session" }, { ".old", "the session before" } };
    for (const auto& s : sessions)
    {
        LogFile f;
        f.mPath = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, APP_NAME + s.ext);
        f.mSendAs = APP_NAME + s.ext;
        f.mWhat = s.what;
        llstat st;
        if (LLFile::stat(f.mPath, &st) == 0 && st.st_size > 0)
        {
            f.mSize = (S64)st.st_size;
            mLogs.push_back(f);
        }
    }

    // The last crash: each run writes static_debug_info.log into its own "dump-<uuid>" folder in the logs folder
    // (llappviewer.cpp setDebugFileNames(LL_PATH_DUMP), lldir.cpp getDumpDir) and a clean exit deletes it
    // (LLAppViewer::removeDumpDir), so a folder left behind is a run that crashed or was killed. The crash handler
    // also copies the log there (wolfcrashreport.cpp; [CRASH REPORTS 2026-10-28]). The minidump (.dmp) is NOT sent
    // with a flea report: it goes only with the automatic crash report, to a staff-only ticket (wolfcrashreport.h).
    const std::string logdir = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, "");
    const std::string this_run = gDirUtilp->dumpDirExists() ? gDirUtilp->getBaseFileName(gDirUtilp->getDumpDir()) : std::string();
    std::string newest;
    time_t newest_time = 0;
    {
        LLDirIterator it(logdir, "dump-*");
        std::string name;
        while (it.next(name))
        {
            const std::string dir = logdir + name;
            llstat st;
            if (name == this_run || !LLFile::isdir(dir) || LLFile::stat(dir + gDirUtilp->getDirDelimiter() + "static_debug_info.log", &st) != 0)
            {
                continue;
            }
            if (st.st_mtime > newest_time)
            {
                newest_time = st.st_mtime;
                newest = dir;
            }
        }
    }
    if (!newest.empty() && time(nullptr) - newest_time <= CRASH_MAX_AGE)
    {
        const std::string when = date_text(newest_time);
        for (const std::string& file : { std::string("static_debug_info.log"), APP_NAME + ".log" })
        {
            LogFile f;
            f.mPath = newest + gDirUtilp->getDirDelimiter() + file;
            f.mSendAs = "crash-" + file;
            f.mWhat = "the crash on " + when;
            llstat st;
            if (LLFile::stat(f.mPath, &st) == 0 && st.st_size > 0)
            {
                f.mSize = (S64)st.st_size;
                mLogs.push_back(f);
            }
        }
    }

    std::string t;
    if (mLogs.empty())
    {
        t = "No viewer logs were found, so only your message will be sent.";
    }
    else
    {
        t = mAttach->get() ? "These will be sent:\n" : "Not attached (tick the box to send them):\n";
        for (const LogFile& f : mLogs)
        {
            t += f.mSendAs + " - " + f.mWhat + ", " + size_text(f.mSize)
                 + (f.mSize > MAX_LOG_BYTES ? " (the last " + size_text(MAX_LOG_BYTES) + " only)" : std::string()) + "\n";
        }
        t += "Session ids and capability links are taken out first. Only the support team can open them.";
    }
    mLogsList->setText(t);
}

void WolfFloaterBugReport::setStatus(const std::string& text, EStatus kind)
{
    // Light red / light green on the dark floater, and the words say it too ("Not sent", "Sent"), never colour alone.
    static const LLUIColor err(LLColor4(1.f, 0.6f, 0.6f, 1.f));
    static const LLUIColor ok(LLColor4(0.79f, 1.f, 0.79f, 1.f));
    static const LLUIColor info(LLColor4::white);
    const LLUIColor& c = kind == EStatus::ERR ? err : kind == EStatus::OK ? ok : info;
    mStatus->setReadOnlyColor(c);
    mStatus->setColor(c);
    mStatus->setText(text);
    mStatus->setVisible(!text.empty());
}

void WolfFloaterBugReport::setSending(bool sending)
{
    mSending = sending;
    mSend->setEnabled(!sending);
    mSend->setLabel(sending ? std::string("Sending...") : std::string("Send"));
    updatePictures();   // [FLEA PICTURES 2026-10-27/] no adding or removing while the report is on its way
}

// [FLEA PICTURES 2026-10-27] The pictures list: one line per picture with its Remove button, or a line saying there are
// none. The two add buttons are greyed out once MAX_PICTURES are added or being prepared.
void WolfFloaterBugReport::updatePictures()
{
    const S32 n = (S32)mPictures.size();
    const bool room = !mSending && n + mPicturesPending < MAX_PICTURES;
    getChild<LLButton>("take_snapshot")->setEnabled(room);
    getChild<LLButton>("add_picture")->setEnabled(room);
    LLTextBox* none = getChild<LLTextBox>("pictures_none");
    none->setVisible(n == 0);
    none->setText(mPicturesPending > 0 ? std::string("Preparing the picture...")
                                       : std::string("No pictures added. They are optional."));
    for (S32 i = 0; i < MAX_PICTURES; ++i)
    {
        LLTextBox* line = getChild<LLTextBox>(llformat("picture_%d", i + 1));
        LLButton* remove = getChild<LLButton>(llformat("remove_picture_%d", i + 1));
        const bool used = i < n;
        line->setVisible(used);
        remove->setVisible(used);
        remove->setEnabled(used && !mSending);
        if (used)
        {
            const Picture& p = mPictures[i];
            line->setText(llformat("%d. ", i + 1) + p.mLabel + llformat(" - %d x %d, ", p.mWidth, p.mHeight)
                          + size_text((S64)p.mData.size()));
            line->setToolTip(line->getText());
        }
    }
}

void WolfFloaterBugReport::onTakeSnapshot()
{
    if (mSending) return;
    if ((S32)mPictures.size() + mPicturesPending >= MAX_PICTURES)
    {
        setStatus(llformat("A flea report can carry %d pictures. Remove one to add another.", MAX_PICTURES), EStatus::ERR);
        return;
    }
    // As LLFloaterReporter::takeNewSnapshot (llfloaterreporter.cpp) does it: "Take a screenshot, but don't draw this
    // floater." The rest of the viewer, menus and HUDs included, is in the picture, since the problem may be there.
    // Window size as LLFileTakeSnapshotToDisk (llviewermenufile.cpp), and its RenderBalanceInSnapshot choice; the
    // signature is llviewerwindow.h rawSnapshot(raw, width, height, keep_window_aspect, is_texture, show_ui, show_hud,
    // do_rebuild, no_post, show_balance).
    LLPointer<LLImageRaw> raw = new LLImageRaw;
    setVisible(false);
    const bool ok = gViewerWindow->rawSnapshot(raw, gViewerWindow->getWindowWidthRaw(), gViewerWindow->getWindowHeightRaw(),
                                               true, false, true, true, false, false,
                                               gSavedSettings.getBOOL("RenderBalanceInSnapshot"));
    setVisible(true);
    if (!ok)
    {
        LL_WARNS("WolfBugReport") << "snapshot failed" << LL_ENDL;
        setStatus("The snapshot could not be taken (the viewer may be short of memory). Try again, or use "
                  "\"Add a picture...\" with a screenshot you saved yourself.", EStatus::ERR);
        return;
    }
    startPreparing("Snapshot taken at " + time_text(time(nullptr)), std::string(), (S8)IMG_CODEC_INVALID, raw);
}

void WolfFloaterBugReport::onAddPicture()
{
    if (mSending) return;
    if ((S32)mPictures.size() + mPicturesPending >= MAX_PICTURES)
    {
        setStatus(llformat("A flea report can carry %d pictures. Remove one to add another.", MAX_PICTURES), EStatus::ERR);
        return;
    }
    // The image picker as File > Upload > Image uses it (llviewermenufile.cpp: startPicker(..., FFLOAD_IMAGE, false)).
    // The callback comes back on the main thread; the floater may have been closed by then.
    const LLHandle<LLFloater> handle = getHandle();
    LLFilePickerReplyThread::startPicker(
        [handle](const std::vector<std::string>& filenames, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (WolfFloaterBugReport* self = static_cast<WolfFloaterBugReport*>(handle.get()))
            {
                self->onPictureFilePicked(filenames);
            }
        },
        LLFilePicker::FFLOAD_IMAGE, false);
}

void WolfFloaterBugReport::onPictureFilePicked(const std::vector<std::string>& filenames)
{
    if (filenames.empty() || filenames[0].empty() || mSending) return;   // the picker was cancelled
    if ((S32)mPictures.size() + mPicturesPending >= MAX_PICTURES)
    {
        setStatus(llformat("A flea report can carry %d pictures. Remove one to add another.", MAX_PICTURES), EStatus::ERR);
        return;
    }
    const std::string& path = filenames[0];
    const std::string name = gDirUtilp->getBaseFileName(path, false);
    // The types FFLOAD_IMAGE offers (llfilepicker.cpp: tga, bmp, jpg, jpeg, png); llimage.cpp getCodecFromExtension maps
    // the lower-case extension (lldir.cpp getExtension) to its codec.
    const EImageCodec codec = LLImageBase::getCodecFromExtension(gDirUtilp->getExtension(path));
    if (codec != IMG_CODEC_PNG && codec != IMG_CODEC_JPEG && codec != IMG_CODEC_BMP && codec != IMG_CODEC_TGA)
    {
        setStatus("Picture not added: " + name + " is not a PNG, JPEG, BMP or TGA picture.", EStatus::ERR);
        return;
    }
    llstat st;
    if (LLFile::stat(path, &st) != 0 || st.st_size <= 0)
    {
        setStatus("Picture not added: " + name + " could not be opened, or is empty.", EStatus::ERR);
        return;
    }
    if ((S64)st.st_size > PICTURE_MAX_FILE_BYTES)
    {
        setStatus("Picture not added: " + name + " is " + size_text((S64)st.st_size) + "; please pick one under "
                  + size_text(PICTURE_MAX_FILE_BYTES) + ".", EStatus::ERR);
        return;
    }
    startPreparing(name, path, (S8)codec, LLPointer<LLImageRaw>());
}

// Scale and encode off the main thread (a 4K snapshot as PNG takes a moment), the way onSend prepares the logs.
void WolfFloaterBugReport::startPreparing(const std::string& label, const std::string& path, S8 codec, LLPointer<LLImageRaw> raw)
{
    ++mPicturesPending;
    updatePictures();
    setStatus("Preparing the picture...", EStatus::INFO);
    const LLHandle<LLFloater> handle = getHandle();
    auto work = [path, codec, raw]() { return path.empty() ? encode_picture(raw) : load_picture_file(path, codec); };
    auto done = [handle, label](PreparedPicture prepared)
    {
        WolfFloaterBugReport* self = static_cast<WolfFloaterBugReport*>(handle.get());
        if (!self) return;
        Picture picture;
        picture.mLabel = label;
        picture.mData.swap(prepared.mData);
        picture.mExt = prepared.mExt;
        picture.mWidth = prepared.mWidth;
        picture.mHeight = prepared.mHeight;
        self->onPictureReady(prepared.mError, picture);
    };
    LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t general_queue = LL::WorkQueue::getInstance("General");
    if (!main_queue || !general_queue || !main_queue->postTo(general_queue, work, done))
    {
        done(work());   // no worker thread: do it here, as onSend does for the logs
    }
}

void WolfFloaterBugReport::onPictureReady(const std::string& error, const Picture& picture)
{
    if (mPicturesPending > 0) --mPicturesPending;
    if (!error.empty() || picture.mData.empty())
    {
        LL_WARNS("WolfBugReport") << "picture not added: " << picture.mLabel << ": " << error << LL_ENDL;
        setStatus("Picture not added: " + (error.empty() ? std::string("it could not be prepared.") : error), EStatus::ERR);
    }
    else if (mSending || (S32)mPictures.size() >= MAX_PICTURES)
    {
        setStatus("Picture not added: the flea report already has its pictures.", EStatus::ERR);
    }
    else
    {
        mPictures.push_back(picture);
        setStatus("Added: " + picture.mLabel + ". It will be sent with your flea report.", EStatus::OK);
    }
    updatePictures();
}


void WolfFloaterBugReport::onSend()
{
    if (mSending) return;
    // Checked on Send, not while typing; the box below says what is missing and the cursor goes to it.
    const std::string category = mCategory->getSelectedValue().asString();
    std::string subject = mSubject->getText();
    std::string body = mBody->getText();
    LLStringUtil::trim(subject);
    LLStringUtil::trim(body);
    if (category.empty())
    {
        setStatus("Not sent: please choose what the flea report is about.", EStatus::ERR);
        mCategory->setFocus(true);
        return;
    }
    if (subject.empty())
    {
        setStatus("Not sent: please give the flea report a short subject.", EStatus::ERR);
        mSubject->setFocus(true);
        return;
    }
    if (body.empty())
    {
        setStatus(is_idea(this) ? std::string("Not sent: please describe your idea.")   // [FLEA REPORT 2026-10-26/]
                                : std::string("Not sent: please say what happened, so we know what to look at."), EStatus::ERR);
        mBody->setFocus(true);
        return;
    }
    // The request carries this login's agent and session ids: only a Wolf Territories login, only to Wolf's own site.
    if (!WolfGrid::isWolfTerritories())
    {
        setStatus("Not sent: flea reports are for Wolf Territories residents.", EStatus::ERR);
        return;
    }
    if (mPicturesPending > 0)   // [FLEA PICTURES 2026-10-27/]
    {
        setStatus("Not sent yet: a picture is still being prepared. Press Send again in a moment.", EStatus::ERR);
        return;
    }

    LLSD fields;
    fields["category"] = category;
    fields["subject"] = subject;
    fields["body"] = body;
    fields["viewer"] = LLVersionInfo::instance().getChannelAndVersion();
    fields["os"] = LLOSInfo::instance().getOSString();
    fields["gpu"] = gGLManager.getRawGLString();
    if (LLViewerRegion* region = gAgent.getRegion())
    {
        fields["region"] = region->getName();
        fields["regionid"] = region->getRegionID().asString();
        const LLVector3 pos = gAgent.getPositionAgent();
        fields["position"] = llformat("%d, %d, %d", (S32)ll_round(pos.mV[VX]), (S32)ll_round(pos.mV[VY]), (S32)ll_round(pos.mV[VZ]));
    }

    mTicketUrl.clear();
    mOpenTicket->setVisible(false);
    setSending(true);
    const LLHandle<LLFloater> handle = getHandle();
    const std::vector<Picture> pictures = mPictures;   // [FLEA PICTURES 2026-10-27/]
    const std::string with_pictures = pictures.empty() ? std::string()
        : llformat(" and %d picture%s", (S32)pictures.size(), pictures.size() == 1 ? "" : "s");
    if (!mAttach->get() || mLogs.empty())
    {
        setStatus("Sending your flea report" + with_pictures + "...", EStatus::INFO);
        LLCoros::instance().launch("WolfBugReport", [handle, fields, pictures]() { sendCoro(handle, fields, {}, pictures); });
        return;
    }

    setStatus("Preparing your logs (taking out private details and compressing them)...", EStatus::INFO);
    const std::vector<LogFile> logs = mLogs;
    const std::vector<std::string> secrets = { gAgentSessionID.asString(), gAgent.getSecureSessionID().asString() };
    LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t general_queue = LL::WorkQueue::getInstance("General");
    auto launch = [handle, fields, pictures, with_pictures](std::vector<std::pair<std::string, std::string>> parts)
    {
        if (WolfFloaterBugReport* self = static_cast<WolfFloaterBugReport*>(handle.get()))
        {
            self->setStatus(with_pictures.empty() ? std::string("Sending your flea report and logs...")
                                                  : "Sending your flea report, logs" + with_pictures + "...", EStatus::INFO);
        }
        LLCoros::instance().launch("WolfBugReport", [handle, fields, parts, pictures]() { sendCoro(handle, fields, parts, pictures); });
    };
    if (!main_queue || !general_queue
        || !main_queue->postTo(general_queue, [logs, secrets]() { return prepare_logs(logs, secrets); }, launch))
    {
        // No worker thread (should not happen once logged in): do it here; a short pause beats sending no logs.
        launch(prepare_logs(logs, secrets));
    }
}

// static
void WolfFloaterBugReport::sendCoro(LLHandle<LLFloater> handle, LLSD fields, std::vector<std::pair<std::string, std::string>> parts,
                                    std::vector<Picture> pictures)
{
    // multipart/form-data (RFC 7578), CRLF line ends. The boundary is random so no log line can contain it.
    LLUUID rnd;
    rnd.generate();
    const std::string boundary = "----WolfBugReport" + rnd.asString();
    LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
    auto put = [&raw](const std::string& s) { raw->append(s.data(), s.size()); };
    for (LLSD::map_const_iterator it = fields.beginMap(); it != fields.endMap(); ++it)
    {
        put("--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + it->first + "\"\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n\r\n" + it->second.asString() + "\r\n");
    }
    for (const auto& p : parts)
    {
        put("--" + boundary + "\r\nContent-Disposition: form-data; name=\"logs[]\"; filename=\"" + p.first + "\"\r\n"
            "Content-Type: application/gzip\r\n\r\n");
        put(p.second);
        put("\r\n");
    }
    // [FLEA PICTURES 2026-10-27] viewerbugreport.php pictures[]. A plain name: a picked file's own name can hold the
    // resident's real name or more, so it stays in this window and is never sent.
    for (size_t i = 0; i < pictures.size(); ++i)
    {
        const Picture& p = pictures[i];
        put("--" + boundary + "\r\nContent-Disposition: form-data; name=\"pictures[]\"; filename=\""
            + llformat("picture-%d.", (S32)i + 1) + p.mExt + "\"\r\n"
            "Content-Type: " + (p.mExt == "png" ? std::string("image/png") : std::string("image/jpeg")) + "\r\n\r\n");
        put(p.mData);
        put("\r\n");
    }
    put("--" + boundary + "--\r\n");

    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfBugReport", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
    options->setTimeout(180);
    LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
    headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "multipart/form-data; boundary=" + boundary);
    headers->append("X-Wolf-Agent", gAgentID.asString());
    headers->append("X-Wolf-Session", gAgentSessionID.asString());
    LLSD result = adapter->postRawAndSuspend(request, REPORT_API, raw, options, headers);

    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(
        result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
    LLSD reply;
    if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
    {
        const LLSD::Binary& bin = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(std::string(bin.begin(), bin.end()), ec);
        if (!ec) reply = LlsdFromJson(v);
    }
    std::string error;
    if (!reply.isMap() || !reply["success"].asBoolean())
    {
        error = json_string(reply, "error");
        if (error.empty())
        {
            error = "the website did not answer properly (" + status.toString() + "). Please try again in a moment, "
                    "or raise a ticket on wolf-grid.com.";
        }
        LL_WARNS("WolfBugReport") << "report not sent: " << status.toString() << " " << error << LL_ENDL;
    }
    WolfFloaterBugReport* self = static_cast<WolfFloaterBugReport*>(handle.get());
    if (!self) return;
    self->onSent(reply, error);
}

void WolfFloaterBugReport::onSent(const LLSD& reply, const std::string& error)
{
    setSending(false);
    if (!error.empty())
    {
        // Stays until the next Send; the text can be selected and copied (a read-only text editor).
        setStatus("Not sent: " + error, EStatus::ERR);
        return;
    }
    const S32 ticket = reply["ticket"].asInteger();
    mTicketUrl = json_string(reply, "url");
    // [FLEA REPORT 2026-10-26/]
    std::string t = llformat(is_idea(this) ? "Sent - thanks for the idea! Your ticket number is #%d. We will reply on wolf-grid.com and by email."
                                           : "Sent - thanks for catching that flea! Your ticket number is #%d. We will reply on wolf-grid.com and by email.",
                             ticket);
    const S32 stored = reply["logsStored"].asInteger();
    if (stored > 0) t += llformat(" %d log file%s attached.", stored, stored == 1 ? "" : "s");
    const S32 pictures = reply["picturesStored"].asInteger();   // [FLEA PICTURES 2026-10-27/]
    if (pictures > 0) t += llformat(" %d picture%s attached.", pictures, pictures == 1 ? "" : "s");
    const LLSD& refused = reply["logsRefused"];
    if (refused.isArray() && refused.size() > 0)
    {
        t += " Some files were not kept:";
        for (const LLSD& r : llsd::inArray(refused)) t += " " + r.asString();
    }
    if (!mTicketUrl.empty()) t += "\n" + mTicketUrl;
    setStatus(t, EStatus::OK);
    mOpenTicket->setVisible(!mTicketUrl.empty());
    // A second click on Send must not raise the same ticket twice: the form is emptied (Send then asks for a subject).
    mSubject->setText(LLStringUtil::null);
    mBody->setText(LLStringUtil::null);
    mCategory->selectFirstItem();
    mPictures.clear();   // [FLEA PICTURES 2026-10-27/] they are on the ticket now
    updatePictures();
}
