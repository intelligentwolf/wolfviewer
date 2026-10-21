/**
 * @file wolffloaterchat.h
 * @brief WolfViewer Wolf Chat: a group's text and voice channels in one window (2026-10-20).
 *
 * Paul: "group calls should be like discord [screenshot] text chat, images from inventory".
 * Laid out as his "WOLF VIEWER" mockup: the group picker and its TEXT CHANNELS / VOICE CHANNELS on the
 * left (who is in each voice room, with their mic and speaker state), your own name with Mic /
 * Deafen / Settings at the bottom; the channel's messages in the middle with Emoji, Picture (from
 * inventory) and Send; Users Online on the right.
 *
 * Messages, channels and who-is-in-which-room live in php/wolfchat.php (WolfStorm web hosts), called
 * with the session pair as WolfAdventure::api is. A voice room is a WebRTC multi-agent room the
 * REGION admits group members to (WebRtcVoiceRegionModule "wolf channel call"). Wolf Territories only.
 *
 * [2026-10-21] The rest of the mockup: the bell (per-channel notifications, unread and @mention badges, toasts while
 * the window is closed - WolfChatNotifier), pinned messages, the GIF button (GIPHY through php/wolfchat.php; a GIF
 * arrives as an animated sprite-sheet texture, WolfChat::GifSheet) and the grid's announcement cards above the
 * message box (WolfChatCards).
 */

#ifndef WOLF_FLOATERCHAT_H
#define WOLF_FLOATERCHAT_H

#include "llfloater.h"
#include "lluictrl.h"
#include "lluiimage.h"
#include "llvoicechannel.h"

#include <boost/signals2/connection.hpp>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

class LLButton;
class LLChatEntry;
class LLCheckBoxCtrl;
class LLRadioGroup;
class LLViewerFetchedTexture;
class LLComboBox;
class LLLineEditor;
class LLScrollListCtrl;
class LLTextBox;
class LLTextEditor;

namespace WolfChat
{
    typedef std::function<void(bool ok, const LLSD& reply, const std::string& error)> api_callback_t;

    /** One php/wolfchat.php request (POST JSON {action, ...}); `done` runs on the main coroutine. */
    void api(const std::string& action, const LLSD& body, api_callback_t done);
    /** [2026-10-21] One php/wolfbridge.php request (the Discord bridge: link_code, links, unlink), as api(). */
    void bridgeApi(const std::string& action, const LLSD& body, api_callback_t done);
    /** The request itself, to either service. */
    void apiTo(const char* url, const std::string& action, const LLSD& body, api_callback_t done);

    /** Opens Wolf Chat on a group (and channel) - the "wolfchat" link Invite People sends. */
    void openChannel(const LLUUID& group, const LLUUID& channel);

    /** [2026-10-21] Paul: "if on wolf grid and you click the chat on a group it should open the new interface, add the
     *  group". Opens Wolf Chat on the group's #group-chat (the standard group IM session) and adds the group to the
     *  server rail if it is not there. Called by LLGroupActions::startIM on Wolf Territories. */
    void openGroupChat(const LLUUID& group);

    /** [2026-10-21] Paul: "on another grid - if it opens a WOLF group it should open the chat". Is this group (by the
     *  name the viewer has for it) a Wolf Territories group? A group of another grid reaches a viewer as "Name @ host"
     *  (OpenSim HGGroupsService.cs:86 names the proxy name + " @ " + the group service's host:port); without that
     *  suffix it is a group of the grid you logged in to. */
    bool isWolfGroup(const std::string& group_name);
    /** [2026-10-21] Said once per group per session when a Wolf group's chat opens as the standard group chat because
     *  Wolf Chat cannot sign you in here (WolfChatGroupChatElsewhere notification). */
    void noteGroupChatElsewhere(const LLUUID& group, const std::string& group_name);

    // ---- the voice room you are in (it stays when the window closes, as a call does)
    bool inRoom();
    LLUUID roomChannel();
    LLUUID roomGroup();
    std::string roomName();
    /** Asks the region for this room's credential and joins it; `done` says how it went. */
    void joinRoom(const LLUUID& group, const LLUUID& channel, const std::string& name,
                  std::function<void(bool ok, const std::string& error)> done);
    void leaveRoom();
    /** [2026-10-21] In the room AND the voice connection is up (the channel's STATE_CONNECTED). */
    bool roomConnected();
    /** [2026-10-21] Why you are not (or no longer) connected, for the window; empty when all is well. */
    std::string roomNote();
    bool micOn();
    void setMic(bool on);
    bool deafened();
    void setDeafened(bool deaf);

    /** [2026-10-21] A GIF as php/wolfchat.php made it (wch_gif_make): `mFrames` frames of mCellW x mCellH pixels,
     *  left to right then top to bottom in `mCols` columns, in a mSheetW x mSheetH texture (asset `mAsset`), one
     *  frame every mDelayMs milliseconds. */
    struct GifSheet
    {
        std::string mId;            // GIPHY's
        LLUUID mAsset;
        std::string mTitle;
        S32 mFrames = 0;
        S32 mCols = 0;
        S32 mRows = 0;
        S32 mCellW = 0;
        S32 mCellH = 0;
        S32 mSheetW = 0;
        S32 mSheetH = 0;
        S32 mDelayMs = 0;

        static GifSheet fromLLSD(const LLSD& gif);
        bool valid() const;
        /** The texture coordinates of `frame` (LLRectf left, top, right, bottom; v = 1 is the top of the picture). */
        LLRectf frameUV(S32 frame) const;
        /** The frame to show now (the clock, so every copy of a GIF plays in step). */
        S32 frameNow() const;
    };

    /** [2026-10-21] Remembers that this account uses Wolf Chat (WolfChatUsed), which lets the bells' background
     *  check run. */
    void markUsed();
    /** [2026-10-21] Starts the background check for new messages (once, at login; llstartup.cpp). */
    void startNotifier();
    /** [2026-10-21] Opens a grid card's link: http(s) in the viewer's own browser, secondlife:// as a clicked link. */
    void openLink(const std::string& url);
}

/** A Wolf Chat voice room as an LLVoiceChannelGroup: the same multi-agent path group and ad-hoc calls use
 *  (llvoicechannel.cpp LLVoiceChannelGroup), with the credential asked for by "wolf channel call". */
class WolfChatVoiceChannel : public LLVoiceChannelGroup
{
public:
    WolfChatVoiceChannel(const LLUUID& channel, const LLUUID& group, const std::string& name);
    void requestChannelInfo() override;

private:
    LLUUID mGroup;
};

/** The left column: group channels, voice rooms with the people in them. Drawn by this control. */
class WolfChatSidebar : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };
    WolfChatSidebar(const Params& p);

    struct Person
    {
        LLUUID mId;
        std::string mName;
        bool mMicOff = false;       // muted (by themselves, or by a moderator)
        bool mDeafened = false;
        bool mSpeaking = false;
        F32 mLevel = 0.f;           // [2026-10-21] voice power 0..1 while speaking (LLVoiceClient::getCurrentPower)
        bool mMutedByMe = false;
    };
    struct Channel
    {
        LLUUID mId;
        std::string mName;
        bool mVoice = false;
        bool mUnread = false;
        S32 mUnreadCount = 0;       // [2026-10-21] from php/wolfchat.php (counted up to 100: "99+")
        S32 mMentions = 0;          // [2026-10-21] unread messages that mention you
        bool mMuted = false;        // [2026-10-21] the bell is set to Nothing
        bool mGroupChat = false;    // [2026-10-21] #group-chat: the standard group IM session (id = the group id)
        bool mRestricted = false;   // [2026-10-21] only some roles can see it (owners are told with a lock)
        bool mBridged = false;      // [2026-10-21] linked to a Discord channel (php/wolfbridge.php links)
        bool mBridgeBroken = false; // that link has stopped (the reason is in the window's header)
        std::vector<Person> mPeople;
    };
    enum EHit { HIT_NONE, HIT_CHANNEL, HIT_ADD_TEXT, HIT_ADD_VOICE, HIT_PERSON };

    void setChannels(const std::vector<Channel>& channels, bool can_manage);
    void setSelected(const LLUUID& text_channel) { mSelected = text_channel; }
    void setJoined(const LLUUID& voice_channel) { mJoined = voice_channel; }
    void setLoading(bool loading) { mLoading = loading; }
    void setClickCallback(std::function<void(EHit, const LLUUID&)> cb) { mClick = cb; }
    /** [2026-10-21] Right-click on a channel row (owners' channel menu); x, y are this control's. */
    void setMenuCallback(std::function<void(const LLUUID&, S32 x, S32 y)> cb) { mMenu = cb; }

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    void onMouseLeave(S32 x, S32 y, MASK mask) override;

private:
    struct Row
    {
        EHit mHit = HIT_NONE;
        LLUUID mId;
        S32 mTop = 0;       // in content space, 0 = top of the list, growing downwards
        S32 mHeight = 0;
        S32 mChannel = -1;  // index into mChannels
        S32 mPerson = -1;   // index into that channel's people
        bool mHeader = false;
        bool mVoiceHeader = false;
    };
    void layout();
    const Row* rowAt(S32 x, S32 y) const;

    std::vector<Channel> mChannels;
    std::vector<Row> mRows;
    bool mCanManage = false;
    bool mLoading = true;
    LLUUID mSelected;
    LLUUID mJoined;
    S32 mScroll = 0;
    S32 mHover = -1;
    std::function<void(EHit, const LLUUID&)> mClick;
    std::function<void(const LLUUID&, S32, S32)> mMenu;
public:
    bool handleRightMouseDown(S32 x, S32 y, MASK mask) override;
};

/** [2026-10-21] Paul: "groups = servers". The far-left rail of the groups the resident added to Wolf Chat (php/wolfchat.php
 *  servers_list), each a tile with the group's insignia or its initials, an unread mark and a red @ count, then "+" =
 *  add groups. Click / Enter opens a group; right-click opens the move / remove menu; with the keyboard Up / Down move
 *  between tiles, Alt+Up / Alt+Down move the group, Delete removes it. Drawn by this control in the Wolf look. */
class WolfChatServerRail : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };
    WolfChatServerRail(const Params& p);

    struct Server
    {
        LLUUID mId;
        std::string mName;
        LLUUID mInsignia;
        S32 mUnread = 0;
        S32 mMentions = 0;
    };
    void setServers(const std::vector<Server>& servers);
    void setSelected(const LLUUID& group) { mSelected = group; }
    void setLoading(bool loading) { mLoading = loading; }
    void setSelectCallback(std::function<void(const LLUUID&)> cb) { mSelect = cb; }
    void setAddCallback(std::function<void()> cb) { mAdd = cb; }
    void setMenuCallback(std::function<void(const LLUUID&, S32 x, S32 y)> cb) { mMenu = cb; }
    void setMoveCallback(std::function<void(const LLUUID&, bool up)> cb) { mMove = cb; }
    void setRemoveCallback(std::function<void(const LLUUID&)> cb) { mRemove = cb; }

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleRightMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    bool handleKeyHere(KEY key, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
    void onMouseLeave(S32 x, S32 y, MASK mask) override;

private:
    LLRect tileRect(S32 i) const;       // i == mServers.size() is the "+" tile
    S32 tileAt(S32 x, S32 y) const;
    void activate(S32 i);

    std::vector<Server> mServers;
    std::vector<LLUIImagePtr> mIcons;
    LLUUID mSelected;
    bool mLoading = true;
    S32 mFocus = 0;
    S32 mHover = -1;
    S32 mScroll = 0;
    std::function<void(const LLUUID&)> mSelect;
    std::function<void()> mAdd;
    std::function<void(const LLUUID&, S32, S32)> mMenu;
    std::function<void(const LLUUID&, bool)> mMove;
    std::function<void(const LLUUID&)> mRemove;
};

/** [2026-10-21] Owners: bridge a text channel to a Discord channel. Asks php/wolfbridge.php link_code for a one-time
 *  code, shows it (and the /wolflink command) with Copy buttons, a 10-minute countdown and the three steps. */
class WolfFloaterChatBridge : public LLFloater
{
public:
    WolfFloaterChatBridge(const LLSD& key);
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;
    void setup(const LLUUID& channel, const std::string& name, bool restricted, std::function<void()> made);

private:
    void requestCode();
    void setStatus(const std::string& text, bool error);

    LLUUID mChannelId;
    std::string mChannelName;
    bool mRestricted = false;
    std::function<void()> mMade;
    LLLineEditor* mCode = nullptr;
    LLLineEditor* mCommand = nullptr;
    LLTextBox* mCountdown = nullptr;
    LLTextBox* mStatus = nullptr;
    F64 mExpires = 0.0;
    bool mBusy = false;
};

/** [2026-10-21] Owners: who can see a channel - everyone in the group, or only members with the roles ticked
 *  (php/wolfchat.php channel_roles). Owners always see every channel. */
class WolfFloaterChatChannelRoles : public LLFloater
{
public:
    WolfFloaterChatChannelRoles(const LLSD& key);
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    /** The channel and its name; `saved` runs after a successful save. */
    void setup(const LLUUID& channel, const std::string& name, std::function<void()> saved);

private:
    void load();
    void save();
    void updateEnabled();
    void setStatus(const std::string& text, bool error);

    LLUUID mChannelId;
    std::string mChannelName;
    std::function<void()> mSaved;
    LLRadioGroup* mWho = nullptr;
    LLView* mRolesPanel = nullptr;
    LLTextBox* mStatus = nullptr;
    LLButton* mSaveBtn = nullptr;
    std::vector<std::pair<LLUUID, LLCheckBoxCtrl*> > mBoxes;
    std::set<LLUUID> mOwnerRole;        // the Owners role: always ticked, cannot be changed
    bool mBusy = false;
};

/** [2026-10-21] Choose which of your groups to add to Wolf Chat's server rail (one or several). */
class WolfFloaterChatAddGroup : public LLFloater
{
public:
    WolfFloaterChatAddGroup(const LLSD& key);
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    /** Called with php/wolfchat.php's server_add answer (servers + the ids added). */
    void setAddedCallback(std::function<void(const LLSD& reply)> cb) { mAdded = cb; }

private:
    void load();
    void add();
    void setStatus(const std::string& text, bool error);

    LLScrollListCtrl* mList = nullptr;
    LLTextBox* mStatus = nullptr;
    LLButton* mAddBtn = nullptr;
    bool mBusy = false;
    std::function<void(const LLSD&)> mAdded;
};

/** A posted picture inside the message list: drawn from the texture, click to see it big. [2026-10-21] Or a GIF:
 *  one cell of its sprite sheet at a time - animated in the channel, its first frame (still) in search, mentions
 *  and pins. */
class WolfChatImage : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Optional<LLUUID> image_id;
        Optional<std::string> image_name;
        Params() : image_id("image_id"), image_name("image_name") {}
    };
    WolfChatImage(const Params& p);
    ~WolfChatImage() override;
    /** Shows this GIF instead of a picture; `animate` false = its first frame only. */
    void setGif(const WolfChat::GifSheet& gif, bool animate);
    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;

private:
    LLUUID mImageId;
    std::string mImageName;
    LLUIImagePtr mImage;
    WolfChat::GifSheet mGif;
    LLPointer<LLViewerFetchedTexture> mSheet;
    bool mAnimate = false;
};

/** [2026-10-21] The grid's announcement cards above the message box (php/wolfchat.php wch_cards_for): icon, title,
 *  subtitle, tag; click or Enter opens the link, the x (or Delete) closes the card for you. Arrow keys move between
 *  cards. Drawn by this control in the Wolf look. */
class WolfChatCards : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };
    WolfChatCards(const Params& p);

    struct Card
    {
        S32 mId = 0;
        std::string mTitle;
        std::string mSubtitle;
        LLUUID mIcon;
        std::string mLink;
        std::string mTag;
    };
    void setCards(const std::vector<Card>& cards);
    bool hasCards() const { return !mCards.empty(); }
    void setOpenCallback(std::function<void(const Card&)> cb) { mOpen = cb; }
    void setCloseCallback(std::function<void(const Card&)> cb) { mClose = cb; }

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    bool handleKeyHere(KEY key, MASK mask) override;
    void onMouseLeave(S32 x, S32 y, MASK mask) override;

private:
    LLRect cardRect(S32 i) const;
    LLRect closeRect(S32 i) const;
    S32 cardAt(S32 x, S32 y) const;

    std::vector<Card> mCards;
    std::vector<LLUIImagePtr> mIcons;
    S32 mFocus = 0;
    S32 mHover = -1;
    bool mHoverClose = false;
    std::function<void(const Card&)> mOpen;
    std::function<void(const Card&)> mClose;
};

/** [2026-10-21] The GIF picker's results: GIPHY stills (served by php/wolfchat.php, never by GIPHY) in a grid.
 *  Click, or arrows + Enter, picks one. */
class WolfChatGifGrid : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };
    WolfChatGifGrid(const Params& p);
    ~WolfChatGifGrid() override;

    struct Result
    {
        std::string mId;
        std::string mTitle;
        std::string mStillUrl;
    };
    void setResults(const std::vector<Result>& results);
    void setLoading(bool loading) { mLoading = loading; }
    void setPickCallback(std::function<void(const Result&)> cb) { mPick = cb; }

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    bool handleKeyHere(KEY key, MASK mask) override;
    void onMouseLeave(S32 x, S32 y, MASK mask) override;

private:
    LLRect cellRect(S32 i) const;
    S32 cellAt(S32 x, S32 y) const;
    void release();

    std::vector<Result> mResults;
    std::vector<LLPointer<LLViewerFetchedTexture> > mStills;
    S32 mFocus = 0;
    S32 mHover = -1;
    bool mLoading = true;
    std::function<void(const Result&)> mPick;
};

/** [2026-10-21] Pick a GIF (GIPHY search; trending when nothing is typed). "Powered by GIPHY" as GIPHY's terms ask. */
class WolfFloaterChatGif : public LLFloater
{
public:
    WolfFloaterChatGif(const LLSD& key);
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void setPickCallback(std::function<void(const std::string& id, const std::string& title)> cb) { mPick = cb; }

private:
    void search(S32 offset);
    void setStatus(const std::string& text, bool error);

    LLLineEditor* mSearch = nullptr;
    WolfChatGifGrid* mGrid = nullptr;
    LLTextBox* mTitle = nullptr;
    LLTextBox* mStatus = nullptr;
    LLButton* mPrevBtn = nullptr;
    LLButton* mNextBtn = nullptr;
    std::string mQuery;
    S32 mOffset = 0;
    S32 mPage = 12;
    bool mMore = false;
    bool mBusy = false;
    S32 mGeneration = 0;
    std::function<void(const std::string&, const std::string&)> mPick;
};

class WolfFloaterChat : public LLFloater
{
public:
    WolfFloaterChat(const LLSD& key);
    ~WolfFloaterChat() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;
    void onFocusReceived() override;    // [2026-10-21] #group-chat: the active IM session while focused (no double toast)
    void onFocusLost() override;
    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type,
                           void* cargo_data, EAcceptance* accept, std::string& tooltip_msg) override;

private:
    struct Message
    {
        S32 mId = 0;
        LLUUID mAgent;
        std::string mName;
        std::string mText;
        LLUUID mImage;
        std::string mImageName;
        WolfChat::GifSheet mGif;    // [2026-10-21] a GIF, when valid()
        std::string mTimeText;      // [2026-10-21] #group-chat: the IM model's own time text (LLIMModel "time")
        bool mPinned = false;       // [2026-10-21]
        S32 mTime = 0;
        LLUUID mChannel;    // search / mentions / pins results only
    };
    enum EView { VIEW_CHANNEL, VIEW_SEARCH, VIEW_MENTIONS, VIEW_PINS };
    static Message messageFromLLSD(const LLSD& m);

public:
    /** [2026-10-21] The channel this window shows, when it is open and on screen (the notifier does not toast it). */
    LLUUID shownChannel() const;

private:

    // loading
    void loadGroups();
    void selectGroup(const LLUUID& group);
    // [2026-10-21] the server rail
    void loadServers();
    void setServers(const LLSD& servers);
    void showEmpty(bool empty);
    void onAddGroup();
    void showServerMenu(const LLUUID& group, S32 x, S32 y);
    void moveServer(const LLUUID& group, bool up);
    void removeServer(const LLUUID& group);
    std::string serverName(const LLUUID& group) const;
    void pushRail();
    // [2026-10-21] #group-chat
    bool groupChatShown() const { return mGroupChatShown; }
    void showGroupChat();
    bool joinGroupChat(std::string& why);
    void appendGroupChat(bool rebuild);
    void sendGroupChat(const std::string& text);
    void onIMMessage(const LLSD& msg);
    void onIMNoUnread(const LLSD& msg);
    bool mInGroupAppend = false;    // appendGroupChat re-entry guard (2026-10-21 crash)
    void loadChannels();
    void loadMessages(bool newer_only);
    void loadOlder();
    void loadOnline();
    void loadPins();
    void showAround(const LLUUID& channel, S32 message_id);

    // showing
    void showChannel(const LLUUID& channel);
    void rebuildLog();
    void appendToLog(const Message& m);
    void updateHeader();
    void updateVoiceBar();
    void refreshSidebar();
    void setStatus(const std::string& text, bool error);

    // doing
    void onSidebarClick(WolfChatSidebar::EHit hit, const LLUUID& id);
    void onSend();
    void onPickImage();
    void postImage(const LLUUID& item_id);
    void onEmoji();
    void onSearch();
    void onMentions();
    void onBackToChannel();
    void onToggleMembers();
    void onHelp();
    void onMic();
    void onDeafen();
    void onVoiceSettings();
    void onLeaveCall();
    void onInvitePeople();
    void onRenameChannel(const LLUUID& channel);
    void onDeleteChannel(const LLUUID& channel);
    // [2026-10-21] owners: the channel menu and "Who can see this channel"
    void showChannelMenu(const LLUUID& channel, S32 x, S32 y);
    void onChannelRoles(const LLUUID& channel);
    // [2026-10-21] the Discord bridge (php/wolfbridge.php)
    void loadBridges();
    void onBridge(const LLUUID& channel);
    void onUnbridge(const LLUUID& channel);
    std::string channelName(const LLUUID& channel) const;
    void deleteMessage(S32 id, bool mine);
    void setPinned(S32 id, bool pin);
    void onNotifySetting();
    void onPickGif();
    void postGif(const std::string& id, const std::string& title);
    void setCards(const LLSD& cards);
    void setCardsShown(bool shown);
    void setGifsAvailable(bool available);
    void askNewChannel(bool voice);
    void joinVoice(const LLUUID& channel);

    WolfChatServerRail* mRail = nullptr;    // [2026-10-21] replaces the group combo box
    LLTextBox* mServerName = nullptr;
    LLView* mEmptyState = nullptr;
    WolfChatSidebar* mSidebar = nullptr;
    LLTextBox* mChannelTitle = nullptr;
    LLTextBox* mStatus = nullptr;
    LLTextEditor* mLog = nullptr;
    LLChatEntry* mInput = nullptr;
    LLLineEditor* mSearchEditor = nullptr;
    LLScrollListCtrl* mOnlineList = nullptr;
    LLTextBox* mOnlineTitle = nullptr;
    LLTextBox* mMeName = nullptr;
    LLTextBox* mVoiceStatus = nullptr;
    LLButton* mSendBtn = nullptr;
    LLButton* mImageBtn = nullptr;
    LLButton* mEmojiBtn = nullptr;
    LLButton* mMicBtn = nullptr;
    LLButton* mDeafenBtn = nullptr;
    LLButton* mLeaveBtn = nullptr;
    LLButton* mInviteBtn = nullptr;
    LLButton* mOlderBtn = nullptr;
    LLButton* mBackBtn = nullptr;
    LLButton* mRenameBtn = nullptr;
    LLButton* mDeleteBtn = nullptr;
    LLButton* mNotifyBtn = nullptr;     // [2026-10-21] the bell
    LLButton* mPinsBtn = nullptr;
    LLButton* mGifBtn = nullptr;
    WolfChatCards* mCards = nullptr;
    LLView* mMembersPanel = nullptr;

    LLUUID mGroup;
    std::string mGroupName;
    bool mCanPost = false;
    bool mCanManage = false;
    std::vector<WolfChatSidebar::Channel> mChannels;
    std::map<LLUUID, S32> mSeen;        // newest message id read, per text channel
    std::map<LLUUID, S32> mLastId;      // newest message id the server has, per text channel
    LLUUID mChannel;                    // the text channel shown
    std::vector<Message> mMessages;     // what the log shows, oldest first
    std::vector<Message> mResults;      // search / mentions
    EView mView = VIEW_CHANNEL;
    LLUUID mPendingChannel;             // from a "wolfchat" link, picked once the channels are in
    bool mHistory = false;              // [2026-10-21] showing the messages around a pin, not the newest
    S32 mHighlight = 0;                 // [2026-10-21] the message jumped to
    bool mGifsAvailable = false;        // [2026-10-21] the grid has GIFs (php/wolfchat.php "gifs")
    bool mCardsShown = false;
    std::map<LLUUID, std::string> mNotifyOf;    // [2026-10-21] each text channel's bell: all / mentions / none

    bool mBusyMessages = false;
    bool mBusyChannels = false;
    bool mBusyOnline = false;
    bool mBusyPost = false;
    F64 mNextMessages = 0.0;
    F64 mNextChannels = 0.0;
    F64 mNextOnline = 0.0;
    S32 mGeneration = 0;                // bumped on every group / channel change: late answers are dropped

    LLHandle<LLFloater> mTexPicker;
    LLHandle<LLFloater> mGifPicker;
    LLSD mCardData;                     // [2026-10-21] the cards shown, as php/wolfchat.php answered them
    std::vector<WolfChatServerRail::Server> mServers;   // [2026-10-21] the rail, in its order
    bool mServersLoaded = false;
    bool mCanModerate = false;          // [2026-10-21] ModerateChat: delete others' messages, pins (channels = owners)
    struct BridgeLink
    {
        std::string mState;             // "active" | "broken"
        std::string mReason;
        std::string mGuild;
        std::string mDiscordChannel;
    };
    std::map<LLUUID, BridgeLink> mBridges;      // [2026-10-21] by Wolf Chat channel
    std::string mBridgeNotice;
    bool mBusyBridges = false;
    F64 mNextBridges = 0.0;
    LLTextBox* mBridgeText = nullptr;
    LLButton* mRelinkBtn = nullptr;
    LLHandle<LLView> mChannelMenu;
    LLUUID mMenuChannel;
    F64 mNextVoiceUi = 0.0;             // [2026-10-21] the voice bar and speaking rings refresh
    bool mBusyServers = false;
    F64 mNextServers = 0.0;
    LLHandle<LLView> mServerMenu;
    LLUUID mMenuServer;                 // the group the open menu is for
    LLHandle<LLFloater> mAddGroup;
    std::map<LLUUID, std::string> mGroupNames;     // all your groups (php/wolfchat.php groups), for link-opened ones
    // [2026-10-21] #group-chat
    bool mGroupChatShown = false;       // the channel shown is #group-chat
    S32 mIMIndex = -1;                  // the newest LLIMModel message index shown
    std::vector<std::string> mGroupChatQueue;   // typed before the session was ready (as FSFloaterIM mQueuedMsgsForInit)
    boost::signals2::connection mIMNewMsg;
    boost::signals2::connection mIMNoUnread;
};

/** A posted picture, big. */
class WolfFloaterChatImage : public LLFloater
{
public:
    WolfFloaterChatImage(const LLSD& key);
    ~WolfFloaterChatImage() override;
    bool postBuild() override;
    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;

private:
    LLUUID mImageId;
    LLUIImagePtr mImage;
    WolfChat::GifSheet mGif;                    // [2026-10-21] a GIF, when valid()
    LLPointer<LLViewerFetchedTexture> mSheet;
    bool mPaused = false;
    S32 mPausedFrame = 0;
};

#endif // WOLF_FLOATERCHAT_H
