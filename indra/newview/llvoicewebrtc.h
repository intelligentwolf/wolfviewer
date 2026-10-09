/**
 * @file llvoicewebrtc.h
 * @brief Declaration of LLWebRTCVoiceClient class which is the interface to the voice client process.
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2023, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */
#ifndef LL_VOICE_WEBRTC_H
#define LL_VOICE_WEBRTC_H

class LLWebRTCProtocolParser;

#include "lliopipe.h"
#include "llpumpio.h"
#include "llchainio.h"
#include "lliosocket.h"
#include "v3math.h"
#include "llframetimer.h"
#include "llviewerregion.h"
#include "llcallingcard.h"   // for LLFriendObserver
#include "lleventcoro.h"
#include "llcoros.h"
#include "llparcel.h"
#include "llmutelist.h"
#include <queue>
#include "boost/json.hpp"

#ifdef LL_USESYSTEMLIBS
# include "expat.h"
#else
# include "expat/expat.h"
#endif
#include "llvoiceclient.h"

// WebRTC Includes
#include <llwebrtc.h>

class LLAvatarName;
class LLVoiceWebRTCConnection;
typedef std::shared_ptr<LLVoiceWebRTCConnection> connectionPtr_t;

extern const std::string WEBRTC_VOICE_SERVER_TYPE;

class LLWebRTCVoiceClient : public LLSingleton<LLWebRTCVoiceClient>,
                            virtual public LLVoiceModuleInterface,
                            public llwebrtc::LLWebRTCDevicesObserver,
                            public LLMuteListObserver,
                            public llwebrtc::LLWebRTCLogCallback
{
    LLSINGLETON(LLWebRTCVoiceClient);
    LOG_CLASS(LLWebRTCVoiceClient);
    virtual ~LLWebRTCVoiceClient();

public:
    void cleanupSingleton() override;
    /// @name LLVoiceModuleInterface virtual implementations
    ///  @see LLVoiceModuleInterface
    //@{
    void init(LLPumpIO *pump) override;    // Call this once at application startup (creates connector)
    void terminate() override;    // Call this to clean up during shutdown

    static bool isShuttingDown() { return sShuttingDown; }

    const LLVoiceVersionInfo& getVersion() override;

    void updateSettings() override; // call after loading settings and whenever they change

    // Returns true if WebRTC has successfully logged in and is not in error state
    bool isVoiceWorking() const override;

    std::string sipURIFromID(const LLUUID &id) const override;
    LLSD getP2PChannelInfoTemplate(const LLUUID& id) const override;

    void setHidden(bool hidden) override;  // virtual

    ///////////////////
    /// @name Logging
    /// @{
    void LogMessage(llwebrtc::LLWebRTCLogCallback::LogLevel level, const std::string& message) override;
    //@}

    /////////////////////
    /// @name Tuning
    //@{
    void tuningStart() override;
    void tuningStop() override;
    bool inTuningMode() override;

    void tuningSetMicVolume(float volume) override;
    void tuningSetSpeakerVolume(float volume) override;
    float tuningGetEnergy(void) override;
    //@}

    /////////////////////
    /// @name Devices
    //@{
    // This returns true when it's safe to bring up the "device settings" dialog in the prefs.
    bool deviceSettingsAvailable() override;
    bool deviceSettingsUpdated() override;  //return if the list has been updated and never fetched,  only to be called from the voicepanel.

    // Requery the WebRTC daemon for the current list of input/output devices.
    // If you pass true for clearCurrentList, deviceSettingsAvailable() will be false until the query has completed
    // (use this if you want to know when it's done).
    // If you pass false, you'll have no way to know when the query finishes, but the device lists will not appear empty in the interim.
    void refreshDeviceLists(bool clearCurrentList = true) override;

    void setCaptureDevice(const std::string& name) override;
    void setRenderDevice(const std::string& name) override;

    LLVoiceDeviceList& getCaptureDevices() override;
    LLVoiceDeviceList& getRenderDevices() override;
    //@}

    void getParticipantList(std::set<LLUUID> &participants) override;
    bool isParticipant(const LLUUID& speaker_id) override;

    // Send a text message to the specified user, initiating the session if necessary.
    // virtual bool sendTextMessage(const LLUUID& participant_id, const std::string& message) const {return false;};

    // Returns true if calling back the session URI after the session has closed is possible.
    // Currently this will be false only for PSTN P2P calls.
    // NOTE: this will return true if the session can't be found.
    bool isSessionCallBackPossible(const LLUUID &session_id) override;

    // WebRTC doesn't preclude text im
    bool isSessionTextIMPossible(const LLUUID &session_id) override { return true; }

    ////////////////////////////
    /// @name Channel stuff
    //@{
    // returns true iff the user is currently in a proximal (local spatial) channel.
    // Note that gestures should only fire if this returns true.
    bool inProximalChannel() override;

    void setNonSpatialChannel(const LLSD& channelInfo, bool notify_on_first_join, bool hangup_on_last_leave) override
    {
        startAdHocSession(channelInfo, notify_on_first_join, hangup_on_last_leave);
    }

    bool setSpatialChannel(const LLSD &channelInfo) override;

    void leaveNonSpatialChannel() override;

    void processChannels(bool process) override;

    void leaveChannel(bool stopTalking);

    bool isCurrentChannel(const LLSD &channelInfo) override;
    bool compareChannels(const LLSD &channelInfo1, const LLSD &channelInfo2) override;
    //@}

    LLVoiceP2POutgoingCallInterface *getOutgoingCallInterface() override { return nullptr; }

    LLVoiceP2PIncomingCallInterfacePtr getIncomingCallInterface(const LLSD &voice_call_info) override { return nullptr; }

    /////////////////////////
    /// @name Volume/gain
    //@{
    void setVoiceVolume(F32 volume) override;
    void setMicGain(F32 volume) override;
    //@}

    /////////////////////////
    /// @name enable disable voice and features
    //@{
    void setVoiceEnabled(bool enabled) override;
    void setMuteMic(bool muted) override;        // Set the mute state of the local mic.
    //@}

    void notifyVoiceConnected() override; // <FS:TJ/> Fix Nearby Voice when changing voice device settings

    //////////////////////////
    /// @name nearby speaker accessors
    std::string getDisplayName(const LLUUID& id) override;
    bool isParticipantAvatar(const LLUUID &id) override;
    bool getIsSpeaking(const LLUUID& id) override;
    bool getIsModeratorMuted(const LLUUID& id) override;
    F32 getCurrentPower(const LLUUID& id) override;        // "power" is related to "amplitude" in a defined way.  I'm just not sure what the formula is...
    F32 getUserVolume(const LLUUID& id) override;
    void setUserVolume(const LLUUID& id, F32 volume) override; // set's volume for specified agent, from 0-1 (where .5 is nominal)
    //@}

    //////////////////
    /// @name LLMuteListObserver
    //@{
    void onChange() override;
    void onChangeDetailed(const LLMute& ) override;
    //@}

    // authorize the user
    void userAuthorized(const std::string &user_id, const LLUUID &agentID) override;


    void OnConnectionEstablished(const std::string& channelID, const LLUUID& regionID);
    // <WolfViewer 2026-10-09> A connection finished its join (VOICE_STATE_SESSION_UP): the
    // moment a make-before-break switch may let go of the session it is replacing.
    void OnConnectionUp(const std::string& channelID, const LLUUID& regionID);
    // </WolfViewer>
    void OnConnectionShutDown(const std::string &channelID, const LLUUID &regionID);
    void OnConnectionFailure(const std::string &channelID,
        const LLUUID &regionID,
        LLVoiceClientStatusObserver::EStatusType status_type = LLVoiceClientStatusObserver::ERROR_UNKNOWN);
    void updatePosition(void); // update the internal position state
    void sendPositionUpdate(bool force); // send the position to the voice server.
    void updateOwnVolume();

    //////////////////////////////
    /// @name Status notification
    //@{
    void addObserver(LLVoiceClientStatusObserver* observer) override;
    void removeObserver(LLVoiceClientStatusObserver* observer) override;
    void addObserver(LLFriendObserver* observer) override;
    void removeObserver(LLFriendObserver* observer) override;
    void addObserver(LLVoiceClientParticipantObserver* observer) override;
    void removeObserver(LLVoiceClientParticipantObserver* observer) override;
    //@}

    //////////////////////////////
    /// @name Devices change notification
    //  LLWebRTCDevicesObserver
    //@{
    void OnDevicesChanged(const llwebrtc::LLWebRTCVoiceDeviceList &render_devices,
                          const llwebrtc::LLWebRTCVoiceDeviceList &capture_devices) override;
    //@}
    void OnDevicesChangedImpl(const llwebrtc::LLWebRTCVoiceDeviceList &render_devices,
                              const llwebrtc::LLWebRTCVoiceDeviceList &capture_devices);

    struct participantState
    {
    public:
        participantState(const LLUUID& agent_id, const LLUUID& region);

        bool isAvatar();

        std::string mURI;
        LLUUID mAvatarID;
        std::string mDisplayName;
        LLFrameTimer mSpeakingTimeout;
        F32 mLevel; // the current audio level of the participant
        F32 mVolume; // the gain applied to the participant
        bool mIsSpeaking;
        bool mIsModeratorMuted;
        LLUUID mRegion;
    };
    typedef std::shared_ptr<participantState> participantStatePtr_t;

    participantStatePtr_t findParticipantByID(const std::string &channelID, const LLUUID &id);
    participantStatePtr_t addParticipantByID(const std::string& channelID, const LLUUID &id, const LLUUID& region);
    void removeParticipantByID(const std::string& channelID, const LLUUID &id, const LLUUID& region);

  protected:

    typedef std::map<const LLUUID, participantStatePtr_t> participantUUIDMap;

    class sessionState
    {
    public:
        typedef std::shared_ptr<sessionState> ptr_t;
        typedef std::weak_ptr<sessionState> wptr_t;

        typedef std::function<void(const ptr_t &)> sessionFunc_t;

        static void addSession(const std::string &channelID, ptr_t& session);
        virtual ~sessionState();

        participantStatePtr_t addParticipant(const LLUUID& agent_id, const LLUUID& region);
        void removeParticipant(const participantStatePtr_t &participant);
        void removeAllParticipants(const LLUUID& region = LLUUID());

        participantStatePtr_t findParticipantByID(const LLUUID& id);

        static ptr_t matchSessionByChannelID(const std::string& channel_id);

        void shutdownAllConnections();
        void revive();

        static void processSessionStates();

        virtual bool processConnectionStates();

        virtual void sendData(const std::string &data);

        void setMuteMic(bool muted);
        void setSpeakerVolume(F32 volume);
        void setUserVolume(const LLUUID& id, F32 volume);

        void setUserMute(const LLUUID& id, bool mute);

        static void for_each(sessionFunc_t func);

        static void reapEmptySessions();
        static void clearSessions();

        bool isEmpty() { return mWebRTCConnections.empty(); }

        // <WolfViewer 2026-10-09> wolf_move hands a live connection from one session to another.
        void wolfAdoptConnection(const connectionPtr_t& connection);
        void wolfDetachConnection(const connectionPtr_t& connection);
        connectionPtr_t wolfMovableConnection(const LLUUID& agent_region);
        bool wolfOwnsConnection(const connectionPtr_t& connection);
        // </WolfViewer>

        virtual bool isSpatial() = 0;
        virtual bool isEstate()  = 0;
        virtual bool isCallbackPossible() = 0;

        std::string mHandle;
        std::string mChannelID;
        std::string mName;

        bool    mMuted;          // this session is muted.
        F32     mSpeakerVolume;  // volume for this session.

        bool        mShuttingDown;

        participantUUIDMap mParticipantsByUUID;

        static bool hasSession(const std::string &sessionID)
        { return sSessions.find(sessionID) != sSessions.end(); }

       bool mHangupOnLastLeave;  // notify observers after the session becomes empty.
       bool mNotifyOnFirstJoin;  // notify observers when the first peer joins.

    protected:
        sessionState();
        std::list<connectionPtr_t> mWebRTCConnections;

    private:

        static std::map<std::string, ptr_t> sSessions;  // canonical list of outstanding sessions.

        static void for_eachPredicate(const std::pair<std::string,
                                      LLWebRTCVoiceClient::sessionState::wptr_t> &a,
                                      sessionFunc_t func);
    };

    typedef std::shared_ptr<sessionState> sessionStatePtr_t;
    typedef std::map<std::string, sessionStatePtr_t> sessionMap;

    class estateSessionState : public sessionState
    {
      public:
        estateSessionState(bool create_connection = true); // <WolfViewer 2026-10-09/> false: wolf_move adopts one
        bool processConnectionStates() override;

        bool isSpatial() override { return true; }
        bool isEstate() override { return true; }
        bool isCallbackPossible() override { return false; }

      private:
        bool isRegionWebRTCEnabled(const LLUUID& regionID);
        // <WolfViewer 2026-10-09> the agent's region the last time we had a connection to it:
        // after a teleport, the connection to this region is the one to wolf_move.
        LLUUID mWolfLastAgentRegion;
        // </WolfViewer>
    };

    class parcelSessionState : public sessionState
    {
      public:
        parcelSessionState(const std::string& channelID, S32 parcel_local_id, bool create_connection = true); // <WolfViewer 2026-10-09/>

        bool isSpatial() override { return true; }
        bool isEstate() override { return false; }
        bool isCallbackPossible() override { return false; }
    };

    class adhocSessionState : public sessionState
    {
    public:
        adhocSessionState(const std::string &channelID,
            const std::string& credentials,
            bool notify_on_first_join,
            bool hangup_on_last_leave);

        bool isSpatial() override { return false; }
        bool isEstate() override { return false; }

        // only p2p-type adhoc sessions allow callback
        bool isCallbackPossible() override { return mNotifyOnFirstJoin && mHangupOnLastLeave; }

        // don't send spatial data to adhoc sessions.
        void sendData(const std::string &data) override { }

    protected:
        std::string mCredentials;
    };


    ///////////////////////////////////////////////////////
    // Private Member Functions
    //////////////////////////////////////////////////////

    static void predSendData(const LLWebRTCVoiceClient::sessionStatePtr_t &session, const std::string& spatial_data);
    static void predUpdateOwnVolume(const LLWebRTCVoiceClient::sessionStatePtr_t &session, F32 audio_level);
    static void predSetMuteMic(const LLWebRTCVoiceClient::sessionStatePtr_t &session, bool mute);
    static void predSetSpeakerVolume(const LLWebRTCVoiceClient::sessionStatePtr_t &session, F32 volume);
    static void predShutdownSession(const LLWebRTCVoiceClient::sessionStatePtr_t &session);
    static void predSetUserMute(const LLWebRTCVoiceClient::sessionStatePtr_t &session, const LLUUID& id, bool mute);
    static void predSetUserVolume(const LLWebRTCVoiceClient::sessionStatePtr_t &session, const LLUUID& id, F32 volume);

    //----------------------------------
    // devices
    void clearCaptureDevices();
    void addCaptureDevice(const LLVoiceDevice& device);

    void clearRenderDevices();
    void addRenderDevice(const LLVoiceDevice& device);
    void setDevicesListUpdated(bool state);

    /////////////////////////////
    // Sending updates of current state
    void setListenerPosition(const LLVector3d &position, const LLVector3 &velocity, const LLQuaternion &rot);
    void setAvatarPosition(const LLVector3d &position, const LLVector3 &velocity, const LLQuaternion &rot);

    LLVector3d getListenerPosition() { return mListenerPosition; }
    LLVector3d getSpeakerPosition() { return mAvatarPosition; }

    void setEarLocation(S32 loc);


    /////////////////////////////
    // Accessors for data related to nearby speakers

    /////////////////////////////
    sessionStatePtr_t findP2PSession(const LLUUID &agent_id);

    sessionStatePtr_t addSession(const std::string &channel_id, sessionState::ptr_t session);
    void deleteSession(const sessionStatePtr_t &session);

    // Does the actual work to get out of the audio session
    void leaveAudioSession();

    friend class LLWebRTCVoiceClientCapResponder;


    void lookupName(const LLUUID &id);
    void onAvatarNameCache(const LLUUID& id, const LLAvatarName& av_name);
    void avatarNameResolved(const LLUUID &id, const std::string &name);
    static void predAvatarNameResolution(const LLWebRTCVoiceClient::sessionStatePtr_t &session, LLUUID id, std::string name);

    boost::signals2::connection mAvatarNameCacheConnection;

private:
    // init or restart the WebRTC device interface.
    void initWebRTC();

    // Coroutine support methods
    //---
    void voiceConnectionCoro();

    //---
    /// Clean up objects created during a voice session.
    void cleanUp();

    LL::WorkQueue::weak_t mMainQueue;

    F32 mTuningMicGain;
    int mTuningSpeakerVolume;
    bool mDevicesListUpdated;            // set to true when the device list has been updated
                                        // and false when the panelvoicedevicesettings has queried for an update status.
    std::string mSpatialSessionCredentials;

    std::string mMainSessionGroupHandle; // handle of the "main" session group.

    sessionStatePtr_t mSession;    // Session state for the current session

    sessionStatePtr_t mNextSession;    // Session state for the session we're trying to join

    // <WolfViewer 2026-10-09> Voice that stays connected and moves you.
    // 1) wolf_move (wolfstorm/wolfvoice/VOICE_MOVE.md): when the primary spatial connection would
    //    be rebuilt (parcel-channel change, teleport) the existing PeerConnection is re-keyed by
    //    wolfvoice into the new room through the new region's ProvisionVoiceAccountRequest cap.
    // 2) If the move is refused: make-before-break. The old spatial session stays up while
    //    mNextSession negotiates, is parked in mPreviousSession once the new one is established,
    //    and is shut down when the new one reaches VOICE_STATE_SESSION_UP (or the cap runs out).
    sessionStatePtr_t mPreviousSession;
    connectionPtr_t   mWolfMovingConnection; // a wolf_move in flight (one at a time)
    LLFrameTimer      mWolfSwitchTimer;      // running while an old session is kept for a switch
    LLFrameTimer      mWolfParcelHoldTimer;  // running while the agent parcel is still another region's
    bool wolfSwitchSpatialSession(const std::string& channelID, S32 parcel_local_id,
                                  const std::function<sessionStatePtr_t(bool)>& make_session);
    void wolfOnParcelMoveResult(const connectionPtr_t& connection, bool moved, const std::string& channelID,
                                S32 parcel_local_id, const std::function<sessionStatePtr_t(bool)>& make_session);
    void wolfCancelStaleSwitch(bool want_estate, const std::string& channelID);
    void wolfRetirePreviousSession();
    void wolfCheckSwitchTimeout();
    bool wolfHoldForParcelData(LLViewerRegion* regionp, LLParcel* parcel);
    void wolfNotifySessionJoined();
  public:
    bool wolfMoveAllowed() const;
    // Estate (neighbour) path: a teleport moves the primary connection instead of rebuilding it.
    bool wolfStartEstateMove(const connectionPtr_t& connection, const LLUUID& region_id);
  private:
    // </WolfViewer>

    llwebrtc::LLWebRTCDeviceInterface *mWebRTCDeviceInterface;

    LLVoiceDeviceList mCaptureDevices;
    LLVoiceDeviceList mRenderDevices;

    bool startEstateSession();
    bool startParcelSession(const std::string& channelID, S32 parcelID);
    bool startAdHocSession(const LLSD &channelInfo, bool notify_on_first_join, bool hangup_on_last_leave);

    bool inSpatialChannel();
    bool inOrJoiningChannel(const std::string &channelID);
    bool inEstateChannel();

    LLSD getAudioSessionChannelInfo();

    void enforceTether();

    void updateNeighboringRegions();
    std::set<LLUUID> getNeighboringRegions() { return mNeighboringRegions; }

    LLVoiceVersionInfo mVoiceVersion;

    bool         mSpatialCoordsDirty;

    LLVector3d   mListenerPosition;
    LLVector3d   mListenerRequestedPosition;
    LLVector3    mListenerVelocity;
    LLQuaternion mListenerRot;

    LLVector3d   mAvatarPosition;
    LLVector3    mAvatarVelocity;
    LLQuaternion mAvatarRot;

    std::set<LLUUID> mNeighboringRegions; // includes current region

    bool         mMuteMic;
    bool         mHidden;       //Set to true during teleport to hide the agent's position.

    enum
    {
        earLocCamera = 0,        // ear at camera
        earLocAvatar,            // ear at avatar
        earLocMixed              // ear at avatar location/camera direction
    };

    S32   mEarLocation;

    float mSpeakerVolume;

    F32   mMicGain;

    bool  mVoiceEnabled;
    bool  mProcessChannels;

    typedef std::set<LLVoiceClientParticipantObserver*> observer_set_t;
    observer_set_t mParticipantObservers;

    void notifyParticipantObservers();

    typedef std::set<LLVoiceClientStatusObserver*> status_observer_set_t;
    status_observer_set_t mStatusObservers;

    void notifyStatusObservers(LLVoiceClientStatusObserver::EStatusType status);

    bool    mIsInTuningMode;
    bool    mIsProcessingChannels;
    bool    mIsCoroutineActive;

    // These variables can last longer than WebRTC in coroutines so we need them as static
    static bool sShuttingDown;

    LLEventMailDrop mWebRTCPump;
};


class LLVoiceWebRTCStats : public LLSingleton<LLVoiceWebRTCStats>
{
    LLSINGLETON(LLVoiceWebRTCStats);
    LOG_CLASS(LLVoiceWebRTCStats);
    virtual ~LLVoiceWebRTCStats();

  private:
    F64SecondsImplicit mStartTime;

    U32 mConnectCycles;

    F64 mConnectTime;
    U32 mConnectAttempts;

    F64 mProvisionTime;
    U32 mProvisionAttempts;

    F64 mEstablishTime;
    U32 mEstablishAttempts;

  public:

    void reset();
    void connectionAttemptStart();
    void connectionAttemptEnd(bool success);
    void provisionAttemptStart();
    void provisionAttemptEnd(bool success);
    void establishAttemptStart();
    void establishAttemptEnd(bool success);
    LLSD read();
};

class LLVoiceWebRTCConnection :
    public llwebrtc::LLWebRTCSignalingObserver,
    public llwebrtc::LLWebRTCDataObserver,
    public std::enable_shared_from_this<LLVoiceWebRTCConnection>
{
  public:
    LLVoiceWebRTCConnection(const LLUUID &regionID, const std::string &channelID);

    virtual ~LLVoiceWebRTCConnection() = 0;

    //////////////////////////////
    /// @name Signaling notification
    //  LLWebRTCSignalingObserver
    //@{
    void OnIceGatheringState(EIceGatheringState state) override;
    void OnIceCandidate(const llwebrtc::LLWebRTCIceCandidate &candidate) override;
    void OnOfferAvailable(const std::string &sdp) override;
    void OnRenegotiationNeeded() override;
    void OnPeerConnectionClosed() override;
    void OnAudioEstablished(llwebrtc::LLWebRTCAudioInterface *audio_interface) override;
    //@}

    /////////////////////////
    /// @name Data Notification
    /// LLWebRTCDataObserver
    //@{
    void OnDataReceived(const std::string &data, bool binary) override;
    void OnDataChannelReady(llwebrtc::LLWebRTCDataInterface *data_interface) override;
    //@}

    void OnDataReceivedImpl(const std::string &data, bool binary);

    void sendJoin();
    void sendData(const std::string &data);

    void processIceUpdates();

    static void processIceUpdatesCoro(connectionPtr_t connection);

    virtual void setMuteMic(bool muted);
    virtual void setSpeakerVolume(F32 volume);

    void setUserVolume(const LLUUID& id, F32 volume);
    void setUserMute(const LLUUID& id, bool mute);

    bool connectionStateMachine();

    virtual bool isSpatial() { return false; }

    LLUUID getRegionID() { return mRegionID; }

    void shutDown()
    {
        mShutDown = true;
    }

    bool isShuttingDown()
    {
        return mShutDown;
    }

    void OnVoiceConnectionRequestSuccess(const LLSD &body);

  protected:
    typedef enum e_voice_connection_state
    {
        VOICE_STATE_ERROR                  = 0x0,
        VOICE_STATE_START_SESSION          = 0x1,
        VOICE_STATE_WAIT_FOR_SESSION_START = 0x2,
        VOICE_STATE_REQUEST_CONNECTION     = 0x4,
        VOICE_STATE_CONNECTION_WAIT        = 0x8,
        VOICE_STATE_SESSION_ESTABLISHED    = 0x10,
        VOICE_STATE_WAIT_FOR_DATA_CHANNEL  = 0x20,
        VOICE_STATE_SESSION_UP             = 0x40,
        VOICE_STATE_SESSION_RETRY          = 0x80,
        VOICE_STATE_DISCONNECT             = 0x100,
        VOICE_STATE_WAIT_FOR_EXIT          = 0x200,
        VOICE_STATE_SESSION_EXIT           = 0x400,
        VOICE_STATE_WAIT_FOR_CLOSE         = 0x800,
        VOICE_STATE_CLOSED                 = 0x1000,
        VOICE_STATE_SESSION_STOPPING       = 0x1F80
    } EVoiceConnectionState;

    EVoiceConnectionState mVoiceConnectionState;
    LL::WorkQueue::weak_t mMainQueue;

    void setVoiceConnectionState(EVoiceConnectionState new_voice_connection_state)
    {
        if (new_voice_connection_state & VOICE_STATE_SESSION_STOPPING)
        {
            // the new state is shutdown or restart.
            mVoiceConnectionState = new_voice_connection_state;
            return;
        }
        if (mVoiceConnectionState & VOICE_STATE_SESSION_STOPPING)
        {
            // we're currently shutting down or restarting, so ignore any
            // state changes.
            return;
        }

        mVoiceConnectionState = new_voice_connection_state;
    }
    EVoiceConnectionState getVoiceConnectionState()
    {
        return mVoiceConnectionState;
    }

    virtual void requestVoiceConnection() = 0;
    static void requestVoiceConnectionCoro(connectionPtr_t connection) { connection->requestVoiceConnection(); }

    static void breakVoiceConnectionCoro(connectionPtr_t connection);

    LLVoiceClientStatusObserver::EStatusType mCurrentStatus;

    LLUUID mRegionID;
    bool   mPrimary;
    // <WolfViewer 2026-10-09> Was LLUUID. wolfvoice hands out "wv-<uuid>" (39 chars), which
    // LLUUID::set() rejects (lluuid.cpp:215-237) and nulls, so every ICE trickle and logout went
    // up with the NULL uuid and wolfvoice never recognised them. Keep the server's value as the
    // LLSD it arrived as and echo it back unchanged (a uuid stays a uuid for Second Life).
    LLSD   mViewerSession;
    // </WolfViewer>
    std::string mChannelID;

    std::string mChannelSDP;
    std::string mRemoteChannelSDP;

    bool mMuted;
    F32  mSpeakerVolume;

    bool mShutDown;
    S32  mOutstandingRequests;

    S32  mRetryWaitPeriod; // number of UPDATE_THROTTLE_SECONDS we've
                           // waited since our last attempt to connect.
    F32  mRetryWaitSecs;   // number of seconds to wait before next retry



    std::vector<llwebrtc::LLWebRTCIceCandidate> mIceCandidates;
    bool                                        mIceCompleted;

    llwebrtc::LLWebRTCPeerConnectionInterface *mWebRTCPeerConnectionInterface;
    llwebrtc::LLWebRTCAudioInterface *mWebRTCAudioInterface;
    llwebrtc::LLWebRTCDataInterface  *mWebRTCDataInterface;

    // <WolfViewer 2026-10-09> Short receive-side fades instead of hard cuts. llwebrtc has no
    // per-connection fade; the per-connection gain hook it does have is
    // LLWebRTCAudioInterface::setReceiveVolume (llwebrtc.cpp LLWebRTCPeerConnectionImpl::
    // setReceiveVolume -> remote track source SetVolume), so the ramp is stepped from the main
    // loop. The receive volume applied is always mSpeakerVolume * mWolfFadeGain.
    F32  mWolfFadeGain       = 0.f;   // 0 = silent, 1 = full mSpeakerVolume
    U32  mWolfFadeGeneration = 0;     // bumps on every new fade; an older ramp stops itself
    bool mWolfFading         = false; // a ramp is in progress
    bool mWolfFadingOut      = false; // the disconnect fade-out has been started
    void wolfApplyReceiveVolume();
    void wolfFadeReceiveTo(F32 target);

    // wolf_move state
    bool mWolfMoving      = false;  // a wolf_move request is in flight for this connection
    bool mWolfMoveRefused = false;  // the server refused a move; this connection is rebuilt the stock way
    LLFrameTimer mWolfOrphanTimer;  // estate path: running while this primary waits to be moved

  public:
    bool wolfIsUp() { return getVoiceConnectionState() == VOICE_STATE_SESSION_UP && !mShutDown; }
    bool wolfIsMoving() const { return mWolfMoving; }
    bool wolfMoveRefused() const { return mWolfMoveRefused; }
    LLFrameTimer& wolfOrphanTimer() { return mWolfOrphanTimer; }
    // Called after a successful wolf_move: we are primary in the new room - re-send the join.
    void wolfRejoinAfterMove();
    // </WolfViewer>
};


class LLVoiceWebRTCSpatialConnection :
    public LLVoiceWebRTCConnection
{
  public:
    LLVoiceWebRTCSpatialConnection(const LLUUID &regionID, S32 parcelLocalID, const std::string &channelID);

    virtual ~LLVoiceWebRTCSpatialConnection();

    void setMuteMic(bool muted) override;

    bool isSpatial() override { return true; }

    // <WolfViewer 2026-10-09> Ask wolfvoice, through region_id's ProvisionVoiceAccountRequest
    // cap, to re-key this live session into (region_id, parcel_local_id). On success the
    // connection's region, parcel and channel become the new ones. done(moved) runs on the
    // main coroutine either way.
    void wolfRequestMove(const LLUUID& region_id, S32 parcel_local_id, const std::string& channel_id,
                         std::function<void(bool)> done);
    // </WolfViewer>

protected:

    void requestVoiceConnection() override;

    S32    mParcelLocalID;
};

class LLVoiceWebRTCAdHocConnection : public LLVoiceWebRTCConnection
{
  public:
    LLVoiceWebRTCAdHocConnection(const LLUUID &regionID, const std::string &channelID, const std::string& credentials);

    virtual ~LLVoiceWebRTCAdHocConnection();

    bool isSpatial() override { return false; }

  protected:
    void requestVoiceConnection() override;

    std::string mCredentials;
};

#define VOICE_ELAPSED LLVoiceTimer(__FUNCTION__);

#endif //LL_WebRTC_VOICE_CLIENT_H

