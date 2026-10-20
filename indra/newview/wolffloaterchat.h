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
 */

#ifndef WOLF_FLOATERCHAT_H
#define WOLF_FLOATERCHAT_H

#include "llfloater.h"
#include "lluictrl.h"
#include "lluiimage.h"
#include "llvoicechannel.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

class LLButton;
class LLChatEntry;
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

    /** Opens Wolf Chat on a group (and channel) - the "wolfchat" link Invite People sends. */
    void openChannel(const LLUUID& group, const LLUUID& channel);

    // ---- the voice room you are in (it stays when the window closes, as a call does)
    bool inRoom();
    LLUUID roomChannel();
    LLUUID roomGroup();
    std::string roomName();
    /** Asks the region for this room's credential and joins it; `done` says how it went. */
    void joinRoom(const LLUUID& group, const LLUUID& channel, const std::string& name,
                  std::function<void(bool ok, const std::string& error)> done);
    void leaveRoom();
    bool micOn();
    void setMic(bool on);
    bool deafened();
    void setDeafened(bool deaf);
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
        bool mMutedByMe = false;
    };
    struct Channel
    {
        LLUUID mId;
        std::string mName;
        bool mVoice = false;
        bool mUnread = false;
        std::vector<Person> mPeople;
    };
    enum EHit { HIT_NONE, HIT_CHANNEL, HIT_ADD_TEXT, HIT_ADD_VOICE, HIT_PERSON };

    void setChannels(const std::vector<Channel>& channels, bool can_manage);
    void setSelected(const LLUUID& text_channel) { mSelected = text_channel; }
    void setJoined(const LLUUID& voice_channel) { mJoined = voice_channel; }
    void setLoading(bool loading) { mLoading = loading; }
    void setClickCallback(std::function<void(EHit, const LLUUID&)> cb) { mClick = cb; }

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
};

/** A posted picture inside the message list: drawn from the texture, click to see it big. */
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
    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;

private:
    LLUUID mImageId;
    std::string mImageName;
    LLUIImagePtr mImage;
};

class WolfFloaterChat : public LLFloater
{
public:
    WolfFloaterChat(const LLSD& key);
    ~WolfFloaterChat() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;
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
        S32 mTime = 0;
        LLUUID mChannel;    // search / mentions results only
    };
    enum EView { VIEW_CHANNEL, VIEW_SEARCH, VIEW_MENTIONS };

    // loading
    void loadGroups();
    void selectGroup(const LLUUID& group);
    void loadChannels();
    void loadMessages(bool newer_only);
    void loadOlder();
    void loadOnline();

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
    void onRenameChannel();
    void onDeleteChannel();
    void deleteMessage(S32 id, bool mine);
    void askNewChannel(bool voice);
    void joinVoice(const LLUUID& channel);

    LLComboBox* mGroupCombo = nullptr;
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

    bool mBusyMessages = false;
    bool mBusyChannels = false;
    bool mBusyOnline = false;
    bool mBusyPost = false;
    F64 mNextMessages = 0.0;
    F64 mNextChannels = 0.0;
    F64 mNextOnline = 0.0;
    S32 mGeneration = 0;                // bumped on every group / channel change: late answers are dropped

    LLHandle<LLFloater> mTexPicker;
};

/** A posted picture, big. */
class WolfFloaterChatImage : public LLFloater
{
public:
    WolfFloaterChatImage(const LLSD& key);
    bool postBuild() override;
    void draw() override;

private:
    LLUUID mImageId;
    LLUIImagePtr mImage;
};

#endif // WOLF_FLOATERCHAT_H
