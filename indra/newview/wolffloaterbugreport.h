/**
 * @file wolffloaterbugreport.h
 * @brief WolfViewer: Report a Problem - send the Wolf Territories support team a ticket, with the viewer's logs.
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

#ifndef WOLF_FLOATERBUGREPORT_H
#define WOLF_FLOATERBUGREPORT_H

#include "llfloater.h"
#include "llimage.h"   // [FLEA PICTURES 2026-10-27/] LLImageRaw
#include <memory>      // [FLEA ATTACHMENTS 2026-11-03/] std::shared_ptr for a video's bytes
#include "llassettype.h"   // [FLEA ATTACHMENTS 2026-11-03/]

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLLineEditor;
class LLTextBox;
class LLTextEditor;

// [WOLF BUG REPORT 2026-10-24] Paul: "build in a bug reporting a little bug icon on the menu that links to tickets and
// allows them to upload their logs". Opened by the status bar's bug icon and Wolf World > Report a Problem...
// The report becomes a support ticket on wolf-grid.com (gridmanager/viewerbugreport.php, the same tickets staff answer
// in Grid Admin); "My tickets" opens the website's ticket page in the browser. With "Attach my viewer logs" (on by
// default) the current and previous session logs, and the details of the last crash if there was one this week, are
// cut to their last part, scrubbed of the session id and capability links, gzipped and sent with it. Wolf Territories
// only: the request carries this login's agent and session ids.
// [FLEA PICTURES 2026-10-27] Jimmy Olsen, ticket #250: "Can u mod the flea report to also allow people to attach pics?"
// Paul: "make the flea report also allow us to add pictures and screenshots". Up to MAX_PICTURES pictures: "Take a
// snapshot" (the screen without this window) or "Add a picture..." (a file). Each is scaled to 1920 px on its longest
// side and encoded as PNG, or JPEG when the PNG is over 2 MB, on the "General" work queue, then sent as pictures[] with
// the report; viewerbugreport.php puts them on the ticket like a website screenshot.
// [FLEA ATTACHMENTS 2026-11-03] Ticket 478, Jimmy Olsen: "Beyond snapshots or pictures, can u add other ways people could
// send report here? 1- notecards (useful if long texts or even scripts). 2- small videos or animations (wav, mp4,
// gifs?)". Paul agreed: notecards (picked from inventory with "Add a notecard...", or dragged onto the window; their
// text is sent, up to MAX_NOTECARD_BYTES each) and short videos or animations (MP4, WebM or GIF only, checked by their
// first bytes, up to MAX_VIDEO_BYTES each). Sent as notecards[] and videos[]; viewerbugreport.php keeps them for the
// support team only and deletes them 90 days after the ticket is closed.
class WolfFloaterBugReport : public LLFloater
{
public:
    WolfFloaterBugReport(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

    // One file the report would send (findLogs) and what it is called in the ticket.
    struct LogFile
    {
        std::string mPath;
        std::string mSendAs;     // e.g. "WolfViewer.log" (".gz" is added when sent)
        std::string mWhat;       // "this session", "the session before", "the last crash (21 Oct)"
        S64 mSize = 0;
    };

    // [FLEA PICTURES 2026-10-27] One picture ready to send: the encoded bytes and what the list shows.
    struct Picture
    {
        std::string mLabel;      // "Snapshot at 05:21" or the picked file's name (shown here only, never sent)
        std::string mData;       // PNG or JPEG bytes
        std::string mExt;        // "png" or "jpg"
        S32 mWidth = 0;
        S32 mHeight = 0;
    };
    static constexpr S32 MAX_PICTURES = 3;

    // [FLEA ATTACHMENTS 2026-11-03] One notecard's text or one video / animation file, ready to send.
    struct Attachment
    {
        bool mIsNotecard = true;
        std::string mLabel;      // what the list shows: the notecard's name, or the picked file's name (never sent)
        std::string mSendAs;     // the multipart file name: "<notecard name>.txt", or "video-<n>.<ext>"
        std::string mMime;       // text/plain; charset=utf-8, video/mp4, video/webm or image/gif
        std::shared_ptr<const std::string> mData;   // shared, so copying the list into the send coroutine copies no bytes
        LLUUID mItemID;          // the inventory item of a notecard (adding the same one twice is refused)
    };
    // viewerbugreport.php VB_MAX_NOTECARDS / VB_MAX_NOTECARD_BYTES / VB_MAX_VIDEOS / VB_MAX_VIDEO_BYTES (ticket 478 contract).
    static constexpr S32 MAX_NOTECARDS = 3;
    static constexpr S32 MAX_NOTECARD_BYTES = 65536;
    static constexpr S32 MAX_VIDEOS = 2;
    static constexpr S64 MAX_VIDEO_BYTES = 25 * 1024 * 1024;
    static constexpr S32 MAX_ATTACHMENTS = MAX_NOTECARDS + MAX_VIDEOS;   // the rows in the floater

    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type, void* cargo_data,
                           EAcceptance* accept, std::string& tooltip_msg) override;

private:
    enum class EStatus { INFO, OK, ERR };

    void findLogs();
    void onSend();
    void setStatus(const std::string& text, EStatus kind);
    void setSending(bool sending);
    void onSent(const LLSD& reply, const std::string& error);
    static void sendCoro(LLHandle<LLFloater> handle, LLSD fields, std::vector<std::pair<std::string, std::string>> parts,
                         std::vector<Picture> pictures, std::vector<Attachment> attachments);
    // [FLEA PICTURES 2026-10-27]
    void onTakeSnapshot();
    void onAddPicture();
    void onPictureFilePicked(const std::vector<std::string>& filenames);
    void startPreparing(const std::string& label, const std::string& path, S8 codec, LLPointer<LLImageRaw> raw);
    void onPictureReady(const std::string& error, const Picture& picture);
    void updatePictures();
    // [FLEA ATTACHMENTS 2026-11-03]
    void onAddNotecard();
    void addNotecard(const LLUUID& item_id);
    void onNotecardLoaded(const LLUUID& item_id, const std::string& name, const LLUUID& asset_id, LLAssetType::EType type, S32 status);
    void onAddVideo();
    void onVideoFilePicked(const std::vector<std::string>& filenames);
    void onVideoReady(const std::string& error, const Attachment& video);
    void updateAttachments();
    S32 countAttachments(bool notecards) const;

    LLComboBox* mCategory = nullptr;
    LLLineEditor* mSubject = nullptr;
    LLTextEditor* mBody = nullptr;
    LLCheckBoxCtrl* mAttach = nullptr;
    LLTextBox* mLogsList = nullptr;
    LLTextEditor* mStatus = nullptr;
    LLButton* mSend = nullptr;
    LLButton* mOpenTicket = nullptr;
    std::vector<LogFile> mLogs;
    std::vector<Picture> mPictures;
    S32 mPicturesPending = 0;   // being scaled and encoded on the "General" queue; they count towards MAX_PICTURES
    std::vector<Attachment> mAttachments;   // [FLEA ATTACHMENTS 2026-11-03/]
    S32 mNotecardsPending = 0;   // being fetched from the grid; they count towards MAX_NOTECARDS
    S32 mVideosPending = 0;      // being read and checked on the "General" queue; they count towards MAX_VIDEOS
    LLHandle<LLFloater> mNotecardPicker;
    std::string mTicketUrl;
    bool mSending = false;
};

#endif // WOLF_FLOATERBUGREPORT_H
