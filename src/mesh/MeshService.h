#pragma once

#include "MeshSupport.h"

#if CROSSPOINT_MESH

#include <MeshCompanionClient.h>
#include <MeshContactBook.h>
#include <MeshLink.h>
#include <MeshSendTracker.h>
#include <MeshThreadLog.h>

#include <cstdint>
#include <memory>

#include "MeshHistory.h"

// Owns the BLE link to the companion radio and the protocol client on top of
// it, keeps the state the Mesh screens show, and writes messages to the
// history on the SD card. Everything here runs on the main loop: start() and
// stop() from the Mesh screen, poll() from loop() in main.cpp, which also
// finishes a teardown that stop() began.
class MeshService final : private meshc::MeshEvents {
 public:
  enum class Phase : uint8_t {
    Off,
    Idle,  // BLE up, no link
    Scanning,
    Connecting,  // connect or pairing in progress
    NeedPin,     // waiting for submitPin()
    Handshake,   // linked; reading the radio's state
    Ready,
    Stopping,  // stop() called, BLE teardown still in progress
  };

  // Why there is no link, for the status line.
  enum class Problem : uint8_t { None, ConnectFailed, PairingFailed, NotCompanion, LinkLost, RadioSilent };

  static constexpr size_t PREVIEW_SIZE = 64;
  // MeshCore companion firmware has at most 40 channel slots.
  static constexpr uint8_t MAX_CHANNELS = 40;
  // Direct-message conversations listed on the Mesh screen, newest first.
  // Older ones stay on the card and open from the contact list.
  static constexpr uint8_t MAX_CONVERSATIONS = 24;
  // Larger than any companion firmware's contact table.
  static constexpr size_t MAX_CONTACTS = 512;

  // One channel as the Mesh screen lists it. The pointers are valid until
  // the next poll(), stop() or sendText().
  struct ChannelView {
    const char* name = "";
    const char* threadKey = "";       // history file key, see MeshHistory.h
    const char* preview = "";         // newest message, empty when there is none
    uint16_t unread = 0;              // received since start() and not yet opened
    uint8_t slot = 0;                 // index on the radio
    const uint8_t* secret = nullptr;  // CHANNEL_SECRET_SIZE bytes
  };

  // One direct-message conversation as the Mesh screen lists it. Pointer
  // validity as for ChannelView.
  struct ConversationView {
    const char* name = "";  // the contact's name, or its key in hex when the radio does not list it
    const char* threadKey = "";
    const char* preview = "";
    uint16_t unread = 0;
  };

  // The open chat screen. Calls come from poll() and sendText(), on the main
  // loop and outside the render lock. stop() calls no listener.
  class ThreadListener {
   public:
    virtual ~ThreadListener() = default;
    // A message joined the open conversation. tag is non-zero for an outgoing
    // message whose delivery state can still change.
    virtual void onThreadMessage(const meshc::StoredMessage& message, uint32_t tag) = 0;
    // The delivery state of the outgoing message with this tag changed;
    // message holds the new state. settled means it will not change again. A
    // listener opened after the send has no row for the message yet.
    virtual void onOutgoingState(uint32_t tag, const meshc::StoredMessage& message, bool settled) = 0;
  };

  static MeshService& getInstance();

  // Bring BLE up and reconnect to the saved radio, if any.
  bool start();
  void stop();
  // Screens that want the link hold the service: the first holder starts it
  // and the link stops a few seconds after the last one lets go, so moving
  // between Home and the Mesh screens keeps it up. False when BLE could not
  // start now; poll() keeps trying while the service is held.
  bool acquire();
  // keepBriefly leaves the link up for a moment, for a screen that hands it
  // to the next one; otherwise the last holder stops it at once.
  void release(bool keepBriefly = true);
  // Stops a link no screen holds now, without the grace period. Settling
  // pending sends writes the history, so call it while the card is ours.
  void stopIfReleased();
  void poll();
  // Blocks until BLE is down or about 2.5 s have passed. Call before deep sleep.
  void shutdownForSleep();

  Phase phase() const;
  Problem problem() const { return lastProblem; }
  // Changes whenever anything the Mesh screen shows has changed.
  uint32_t revision() const { return rev; }

  bool startScan();
  uint8_t deviceCount() const { return MESH_LINK.deviceCount(); }
  bool device(uint8_t index, MeshLinkDevice& out) const { return MESH_LINK.device(index, out); }
  bool connectTo(const MeshLinkDevice& device);
  bool connectSaved();
  bool submitPin(uint32_t pin);
  // Drop the link and stop reconnecting until the user connects again.
  void disconnect();
  // True from disconnect() until the next connect or scan, also across a stop.
  bool disconnectedByUser() const { return userDisconnected; }
  void forgetSavedRadio();
  bool sendAdvert();

  const meshc::DeviceInfo& deviceInfo() const { return client.deviceInfo(); }
  const meshc::SelfInfo& selfInfo() const { return client.selfInfo(); }
  uint16_t batteryMilliVolts() const { return radioMilliVolts; }

  // The radio's contacts, in the order it reported them.
  size_t contactCount() const;
  // How many contacts the table can hold for the radio last linked.
  size_t contactCapacity() const;
  bool contact(size_t index, meshc::ContactEntry& out) const;
  // The contact whose key starts with these KEY_PREFIX_SIZE bytes.
  bool findContact(const uint8_t* keyPrefix, meshc::ContactEntry& out) const;
  // Changes whenever a contact was added, removed or updated.
  uint32_t contactsRevision() const { return contactsRev; }
  // Asks the radio to mark or unmark a contact as a favourite. The mark is
  // the radio's own, shared with the MeshCore apps. The contact list changes
  // once the radio has saved it. False when there is no link, the contact is
  // unknown, or an earlier change is still under way.
  bool setFavorite(const uint8_t* keyPrefix, bool favorite);
  // True from an accepted setFavorite() until the radio has answered.
  bool favoriteBusy() const;
  // True once after a change the radio did not save.
  bool takeFavoriteFailure();

  // Direct-message conversations, most recently active first.
  uint8_t conversationCount() const;
  bool conversation(uint8_t position, ConversationView& out) const;

  // Channels the radio has configured. Slot order on the first link; a
  // channel that appears after a reconnect is added at the end.
  uint8_t channelCount() const;
  bool channel(uint8_t position, ChannelView& out) const;

  // Name the conversation shown on screen. Its messages go to the listener
  // and no longer count as unread. One conversation can be open at a time.
  void openThread(const char* threadKey, ThreadListener& listener);
  void closeThread(const ThreadListener& listener);
  // Sends text to the channel or contact whose history is threadKey. The
  // message always reaches the open listener and the history, marked as
  // failed when the radio cannot take it. False when the text is empty or
  // too long.
  bool sendText(const char* threadKey, const char* text, size_t textLen);
  // Longest text in bytes for that conversation; the radio cuts anything longer.
  size_t maxTextBytes(const char* threadKey) const;
  // The radio's clock in epoch seconds, or 0 when no clock is known.
  uint32_t now() const;

  // Messages received since start() and not yet opened, in every channel and
  // conversation.
  uint16_t unreadTotal() const;
  // The newest conversation with unread messages. Pointer validity as for
  // ChannelView.
  struct UnreadView {
    const char* name = "";
    const char* preview = "";
    uint16_t unread = 0;
  };
  bool latestUnread(UnreadView& out) const;
  // Up to max conversations and channels with unread messages, the one that
  // had the newest message first. Returns how many were written.
  uint8_t recentUnread(UnreadView* out, uint8_t max) const;
  void markRead(const char* threadKey);
  void markAllRead();
  // Deletes the conversation's history from the card. False when the file
  // could not be removed.
  bool clearHistory(const char* threadKey);

  // ---- Requests to the radio and to other nodes (MeshServiceTools.cpp) ----
  // One runs at a time; a screen starts one and polls requestState().
  enum class Request : uint8_t { None, Settings, JoinChannel, LeaveChannel, Login, Status, Discover, Details };
  enum class RequestState : uint8_t { Idle, Busy, Done, Failed };
  // The state of the latest request of this kind; Idle when another kind ran since.
  RequestState requestState(Request kind) const;
  // Forgets a finished request so its result is not reported again.
  void clearRequest();
  bool requestBusy() const { return session && session->requestStatus == RequestState::Busy; }

  // Radio settings. Each writes one setting and reads the radio's details again.
  bool setNodeName(const char* name);
  bool setTxPower(int8_t dbm);
  bool setRadioParams(uint32_t freqKhz, uint32_t bwHz, uint8_t spreadingFactor, uint8_t codingRate);
  bool setPosition(int32_t latE6, int32_t lonE6);
  bool setShareLocation(bool share);

  // Channel slots the radio has, and the first one no channel uses; -1 when full.
  uint8_t channelSlots() const;
  int freeChannelSlot() const;
  // The secret every MeshCore app uses for the "Public" channel.
  static const uint8_t PUBLIC_CHANNEL_SECRET[meshc::CHANNEL_SECRET_SIZE];
  // A hashtag channel's secret: the first 16 bytes of SHA-256 of its name,
  // "#" included. Anyone who knows the name can read it.
  static void hashtagSecret(const char* name, uint8_t* out);
  // True when a channel with this secret is already on the radio.
  bool hasChannel(const uint8_t* secret) const;
  bool joinChannel(const char* name, const uint8_t* secret);
  bool leaveChannel(uint8_t slot);

  // Repeaters and room servers. An empty password logs in as a guest.
  bool login(const uint8_t* keyPrefix, const char* password);
  // True after a login this session that the server accepted.
  bool isLoggedIn(const uint8_t* keyPrefix, bool* isAdmin = nullptr) const;
  bool requestStatus(const uint8_t* keyPrefix);
  // The last status answer; false before one arrived for this server.
  bool lastStatus(const uint8_t* keyPrefix, meshc::ServerStatus& out) const;

  // Asks the repeaters in range to answer; answers gather for a few seconds.
  struct NearbyNode {
    uint8_t keyPrefix[meshc::KEY_PREFIX_SIZE] = {0};
    char name[meshc::NAME_FIELD_SIZE + 1] = {0};  // the key in hex when not a contact
    uint8_t type = 0;
    int8_t snrHereQuarterDb = 0;
    int8_t snrThereQuarterDb = 0;
    int8_t rssiHere = 0;
  };
  static constexpr uint8_t MAX_NEARBY = 16;
  // False when the radio's firmware cannot do it (before version 8).
  bool canDiscover() const { return client.deviceInfo().firmwareVer >= 8; }
  bool discoverNearby();
  // Stops waiting for answers, so another request can start.
  void endDiscovery();
  uint8_t nearbyCount() const;
  bool nearby(uint8_t index, NearbyNode& out) const;

  // Reads one contact's whole record (position, last advert); see details().
  bool requestDetails(const uint8_t* keyPrefix);
  bool details(meshc::Contact& out) const;

 private:
  MeshService();

  static bool sendFrame(void* ctx, const uint8_t* frame, size_t len);
  void onLinkStateChanged(MeshLinkState from, MeshLinkState to, uint32_t nowMs);
  void scheduleReconnect(uint32_t nowMs);
  void touch() { rev++; }

  // meshc::MeshEvents
  void onSelfInfo(const meshc::SelfInfo& info) override;
  void onDeviceTime(uint32_t epochSecs) override;
  void onReady() override;
  void onHandshakeFailed(uint8_t cmd, meshc::FailReason reason) override;
  void onContactsBegin(uint32_t count) override;
  void onContact(const meshc::Contact& contact) override;
  void onContactDeleted(const uint8_t* publicKey) override;
  void onContactFetched(uint32_t tag, const meshc::Contact& contact) override;
  void onCommandDone(uint8_t cmd, uint32_t tag) override;
  void onChannel(const meshc::Channel& channel) override;
  void onMessage(const meshc::Message& message) override;
  void onDirectAccepted(uint32_t tag, const meshc::SentInfo& info) override;
  void onChannelAccepted(uint32_t tag) override;
  void onDeliveryConfirmed(uint32_t ackCode, uint32_t roundTripMs) override;
  void onCommandFailed(uint8_t cmd, uint32_t tag, meshc::FailReason reason, uint8_t errCode) override;
  void onBattery(const meshc::BatteryInfo& info) override;
  void onChannelEmpty(uint8_t index) override;
  void onRequestSent(uint8_t cmd, uint32_t tag, const meshc::SentInfo& info) override;
  void onLoginResult(const meshc::LoginResult& result) override;
  void onServerStatus(const meshc::ServerStatus& status) override;
  void onDiscoverReply(const meshc::DiscoverReply& reply) override;
  // Request bookkeeping, MeshServiceTools.cpp. True when the event was the request's.
  bool beginRequest(Request kind, uint32_t tag, const uint8_t* keyPrefix, uint32_t timeoutMs);
  void finishRequest(bool ok);
  bool requestCommandDone(uint8_t cmd, uint32_t tag);
  bool requestCommandFailed(uint32_t tag);
  bool requestContactFetched(uint32_t tag, const meshc::Contact& contact);
  void pollRequest(uint32_t nowMs);
  void requestChannelRead(uint8_t slot, bool present);

  struct ChannelEntry {
    uint8_t slot = 0;  // index on the radio
    // False from a new link until the radio reports the channel again. An
    // entry kept across a reconnect holds on to its unread count.
    bool live = false;
    uint16_t unread = 0;
    uint32_t unreadSeq = 0;  // when it last got an unread message; see nextUnreadSeq
    uint8_t secret[meshc::CHANNEL_SECRET_SIZE] = {0};
    char name[meshc::NAME_FIELD_SIZE + 1] = {0};
    char threadKey[meshhistory::THREAD_KEY_SIZE] = {0};
    char preview[PREVIEW_SIZE] = {0};
  };

  static constexpr uint8_t SEEN_MESSAGES = 4;

  struct Conversation {
    uint8_t keyPrefix[meshc::KEY_PREFIX_SIZE] = {0};
    uint16_t unread = 0;
    uint32_t unreadSeq = 0;
    uint32_t lastActivity = 0;  // epoch seconds of the newest message
    // The newest incoming messages. A sender that missed our ack sends the
    // same message again, and the radio passes every copy on.
    uint32_t seenTimestamps[SEEN_MESSAGES] = {0};
    uint32_t seenHashes[SEEN_MESSAGES] = {0};
    uint8_t seenNext = 0;
    char name[meshc::NAME_FIELD_SIZE + 1] = {0};
    char threadKey[meshhistory::THREAD_KEY_SIZE] = {0};
    char preview[PREVIEW_SIZE] = {0};
  };

  // An outgoing message whose delivery state can still change: waiting for
  // the radio to take it or, for a direct message, for the recipient's ack.
  struct PendingSend {
    uint32_t tag = 0;           // 0 = free
    bool accepted = false;      // the radio took at least one try
    uint32_t recordOffset = 0;  // its record in the history; 0 = not written yet
    char threadKey[meshhistory::THREAD_KEY_SIZE] = {0};
    meshc::StoredMessage message;
  };

  // The client holds at most its queue plus the one command in flight; the
  // tracker's messages wait for acks on top of that.
  static constexpr uint8_t MAX_PENDING_SENDS =
      meshc::MeshCompanionClient::QUEUE_CAPACITY + 1 + meshc::DirectSendTracker::CAPACITY;

  // Allocated from start() to stop(): about 12 KB, plus the contact table,
  // that an idle reader should not hold.
  struct Session {
    ChannelEntry channels[MAX_CHANNELS];
    uint8_t channelCount = 0;
    Conversation conversations[MAX_CONVERSATIONS];  // most recently active first
    uint8_t conversationCount = 0;
    PendingSend pending[MAX_PENDING_SENDS];
    meshc::DirectSendTracker tracker;
    meshc::StoredMessage scratch;  // message being received or composed
    // Sized from the radio's contact limit when it first lists its contacts.
    std::unique_ptr<meshc::ContactEntry[]> contactStorage;
    meshc::ContactBook contacts;
    bool contactsListed = false;  // on this link
    bool contactsFullLogged = false;
    // The favourite change under way: the contact is read, then written back
    // with one flag changed. favoriteTag 0 = none.
    uint32_t favoriteTag = 0;
    bool favoriteWanted = false;
    meshc::Contact favoriteContact;
    uint32_t nextUnreadSeq = 1;

    // The request under way or last finished; see requestState().
    Request request = Request::None;
    RequestState requestStatus = RequestState::Idle;
    uint32_t requestTag = 0;
    uint32_t requestDeadlineMs = 0;
    uint8_t requestKey[meshc::KEY_PREFIX_SIZE] = {0};
    uint8_t requestSlot = 0;
    // Servers this session logged in to.
    static constexpr uint8_t MAX_LOGINS = 8;
    uint8_t loginKeys[MAX_LOGINS][meshc::KEY_PREFIX_SIZE] = {};
    bool loginAdmin[MAX_LOGINS] = {};
    uint8_t loginCount = 0;
    meshc::ServerStatus status;
    bool hasStatus = false;
    NearbyNode nearby[MAX_NEARBY];
    uint8_t nearbyCount = 0;
    meshc::Contact details;
    bool hasDetails = false;
  };

  uint32_t takeTag();

  void endSession();
  static void addSavedConversation(void* ctx, const char* threadKey);
  Conversation* findConversation(const uint8_t* keyPrefix);
  // The conversation with this contact, created if needed and moved to the
  // front of the list. The least recently active one makes room.
  Conversation* touchConversation(const uint8_t* keyPrefix);
  static bool seenBefore(const Conversation& conversation, uint32_t timestamp, uint32_t hash);
  static void markSeen(Conversation& conversation, uint32_t timestamp, uint32_t hash);
  PendingSend* findPending(uint32_t tag);
  // Shows the message in its channel's or conversation's row.
  void noteMessage(const char* threadKey, const meshc::StoredMessage& message);
  // Gives the message its final delivery state, in the history and on screen.
  void settleSend(PendingSend& pending, meshc::Delivery delivery);
  void settleAllSends();
  void resendDirect(uint32_t tag, bool clearRoute);
  ChannelEntry* findChannelBySlot(uint8_t slot);
  ChannelEntry* findChannelByKey(const char* threadKey);
  // The entry a reported channel goes into: the one kept from the last link
  // with this history, else a new one, else one whose channel is gone.
  // nullptr when every entry is in use.
  ChannelEntry* claimChannelEntry(const char* threadKey);
  void dropStaleChannels();
  bool isOpenThread(const char* threadKey) const;
  void handleChannelMessage(const meshc::Message& message);
  void handleDirectMessage(const meshc::Message& message);
  // Writes a message that will not change again to the history and shows it.
  void storeMessage(const char* threadKey, const meshc::StoredMessage& message);

  meshc::MeshCompanionClient client;
  MeshLinkState lastLinkState = MeshLinkState::Off;
  Problem lastProblem = Problem::None;
  bool stopping = false;
  bool handshakeFailed = false;
  uint8_t holders = 0;
  bool stopPending = false;  // the last holder let go; stop at stopAtMs
  uint32_t stopAtMs = 0;
  uint32_t nextStartMs = 0;
  uint8_t heldStartFailures = 0;
  bool userDisconnected = false;
  uint32_t rev = 0;

  // Reconnect to the saved radio after a lost link or a failed connect.
  bool autoReconnect = false;
  uint8_t reconnectAttempts = 0;
  uint32_t reconnectAtMs = 0;

  // The radio a connect was last requested for; saved once the link is ready.
  MeshLinkDevice pendingDevice{};

  uint32_t nextBatteryPollMs = 0;
  uint16_t radioMilliVolts = 0;
  uint32_t contactsRev = 0;
  bool favoriteFailed = false;

  std::unique_ptr<Session> session;
  ThreadListener* threadListener = nullptr;
  char openThreadKey[meshhistory::THREAD_KEY_SIZE] = {0};
  uint32_t nextTag = 1;

  // The radio's clock: its value at radioEpochAtMs on our millis() clock.
  uint32_t radioEpoch = 0;
  uint32_t radioEpochAtMs = 0;
  uint32_t requestedEpoch = 0;  // the time start() asked the radio to take
};

#define MESH_SERVICE MeshService::getInstance()

#endif  // CROSSPOINT_MESH
