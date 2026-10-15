#pragma once

#include "MeshFrames.h"

namespace meshc {

enum class FailReason : uint8_t { RadioError, Timeout, LinkDown };

// Callbacks from MeshCompanionClient. All run inside onFrame(), poll() or
// reset(), on the caller's thread. References are valid only during the call.
class MeshEvents {
 public:
  virtual ~MeshEvents() = default;

  virtual void onDeviceInfo(const DeviceInfo&) {}
  virtual void onSelfInfo(const SelfInfo&) {}
  virtual void onDeviceTime(uint32_t /*epochSecs*/) {}
  // The handshake finished; user commands are accepted from here on.
  virtual void onReady() {}
  // The radio did not answer the first handshake commands or a channel
  // read. The client stops until the next start().
  virtual void onHandshakeFailed(uint8_t /*cmd*/, FailReason) {}

  virtual void onContactsBegin(uint32_t /*count*/) {}
  virtual void onContact(const Contact&) {}
  // mostRecentLastMod is the 'since' value to pass to the next start().
  virtual void onContactsEnd(uint32_t /*mostRecentLastMod*/) {}
  // An advert from a node that is not in the radio's contact list.
  virtual void onDiscoveredContact(const Contact&) {}
  virtual void onContactDeleted(const uint8_t* /*publicKey*/) {}
  // The answer to requestContact(): the radio's whole record of one contact.
  virtual void onContactFetched(uint32_t /*tag*/, const Contact&) {}
  virtual void onContactsFull() {}

  virtual void onChannel(const Channel&) {}
  // A channel read with readChannel() came back empty: the slot is free.
  virtual void onChannelEmpty(uint8_t /*index*/) {}
  virtual void onMessage(const Message&) {}

  // The radio queued a direct message for sending. Delivery is confirmed
  // later by onDeliveryConfirmed() with the same ack code.
  virtual void onDirectAccepted(uint32_t /*tag*/, const SentInfo&) {}
  // Channel messages have no delivery receipt; this is the last event.
  virtual void onChannelAccepted(uint32_t /*tag*/) {}
  virtual void onDeliveryConfirmed(uint32_t /*ackCode*/, uint32_t /*roundTripMs*/) {}

  virtual void onBattery(const BatteryInfo&) {}
  // A login or status request went out; its answer is a push that may never
  // come. timeoutMs is the radio's estimate of the round trip.
  virtual void onRequestSent(uint8_t /*cmd*/, uint32_t /*tag*/, const SentInfo&) {}
  virtual void onLoginResult(const LoginResult&) {}
  virtual void onServerStatus(const ServerStatus&) {}
  virtual void onDiscoverReply(const DiscoverReply&) {}
  virtual void onCommandDone(uint8_t /*cmd*/, uint32_t /*tag*/) {}
  virtual void onCommandFailed(uint8_t /*cmd*/, uint32_t /*tag*/, FailReason, uint8_t /*errCode*/) {}
};

// Protocol state machine for one companion radio. It owns no transport and no
// clock: the caller passes a send function and the current time in ms, so the
// same code runs on the device and in host tests.
//
// One command is in flight at a time. The radio gives no sequence numbers, so
// a reply is matched to the in-flight command by its code alone.
class MeshCompanionClient {
 public:
  enum class Stage : uint8_t { Idle, DeviceQuery, AppStart, GetTime, SetTime, Contacts, Channels, Sync, Ready, Failed };

  // Returns false when the link cannot take a frame right now; the client
  // retries on the next poll().
  using SendFn = bool (*)(void* ctx, const uint8_t* frame, size_t len);

  struct StartOptions {
    const char* appName = "CrossPoint";
    // Epoch seconds to set on the radio, or 0 to leave its clock alone.
    uint32_t setTimeEpoch = 0;
    // Only contacts modified after this value are fetched; 0 fetches all.
    uint32_t contactsSince = 0;
  };

  static constexpr uint32_t COMMAND_TIMEOUT_MS = 5000;
  static constexpr uint8_t QUEUE_CAPACITY = 8;
  static constexpr uint8_t HANDSHAKE_RETRIES = 2;
  // Slots probed when the radio does not report its channel count.
  static constexpr uint8_t FALLBACK_CHANNEL_SLOTS = 8;

  MeshCompanionClient(SendFn send, void* sendCtx, MeshEvents& events);

  // Begin the handshake on a link that is connected and subscribed.
  void start(const StartOptions& options, uint32_t nowMs);
  // The link dropped. Pending user commands fail with FailReason::LinkDown.
  void reset();
  void onFrame(const uint8_t* frame, size_t len, uint32_t nowMs);
  // Drives timeouts and sends the next command. Call every main-loop pass.
  void poll(uint32_t nowMs);

  Stage stage() const { return currentStage; }
  bool isReady() const { return currentStage == Stage::Ready; }
  bool isIdle() const { return !inFlight && queueCount == 0; }
  const DeviceInfo& deviceInfo() const { return radioInfo; }
  const SelfInfo& selfInfo() const { return nodeInfo; }

  // User commands. Each returns false when the handshake has not finished,
  // the queue is full, or an argument is invalid.
  bool sendDirect(const uint8_t* keyPrefix, const char* text, size_t textLen, uint32_t timestamp, uint8_t attempt,
                  uint32_t tag);
  bool sendChannel(uint8_t channelIndex, const char* text, size_t textLen, uint32_t timestamp, uint32_t tag);
  bool requestBattery();
  bool sendSelfAdvert(bool flood);
  bool setAdvertName(const char* name, uint32_t tag = 0);
  bool setTxPower(int8_t dbm, uint32_t tag = 0);
  bool removeContact(const uint8_t* publicKey);
  // Reads one contact; onContactFetched() gets it, or onCommandFailed().
  bool requestContact(const uint8_t* publicKey, uint32_t tag);
  // Writes a contact back; onCommandDone() or onCommandFailed() follows.
  bool updateContact(const Contact& contact, uint32_t tag);
  bool resetPath(const uint8_t* publicKey);
  bool setRadioParams(uint32_t freqKhz, uint32_t bwHz, uint8_t spreadingFactor, uint8_t codingRate, uint32_t tag);
  bool setAdvertPosition(int32_t latE6, int32_t lonE6, uint32_t tag);
  bool setOtherParams(const SelfInfo& self, uint32_t tag);
  // Reads the radio's own details again; onSelfInfo() follows.
  bool refreshSelfInfo();
  // Writes a channel slot; an empty name and zero secret clear it.
  bool setChannel(const Channel& channel, uint32_t tag);
  // onChannel() or onChannelEmpty() follows.
  bool readChannel(uint8_t index);
  bool login(const uint8_t* publicKey, const char* password, uint32_t tag);
  bool requestStatus(const uint8_t* publicKey, uint32_t tag);
  bool logout(const uint8_t* publicKey);
  // False when the radio's firmware is older than version 8.
  bool discover(uint8_t filter, uint32_t tag);

  // Fetch changed contacts, or drain waiting messages, once the queue is empty.
  void requestContactsRefresh() { contactsDirty = true; }
  void requestSync() { syncWanted = true; }

 private:
  enum class Outcome : uint8_t { Ok, RadioError, Timeout };

  struct Queued {
    uint8_t frame[MAX_FRAME_SIZE];
    uint8_t len;
    uint32_t tag;
  };

  SendFn send;
  void* sendCtx;
  MeshEvents& events;

  Queued queue[QUEUE_CAPACITY];
  uint8_t queueHead = 0;
  uint8_t queueCount = 0;

  bool inFlight = false;
  bool inFlightUser = false;
  uint8_t inFlightCmd = 0;
  uint32_t inFlightTag = 0;
  // The contact an in-flight requestContact() asked for.
  uint8_t requestedKey[PUB_KEY_SIZE] = {0};
  uint32_t deadlineMs = 0;

  Stage currentStage = Stage::Idle;
  uint8_t stageRetries = 0;
  uint8_t channelCursor = 0;
  bool syncGotMessage = false;
  bool syncWanted = false;
  bool contactsDirty = false;

  uint32_t contactsSince = 0;
  uint32_t setTimeEpoch = 0;
  uint32_t startMs = 0;
  char appName[NAME_FIELD_SIZE + 1] = {0};

  DeviceInfo radioInfo;
  SelfInfo nodeInfo;

  Queued* reserveSlot();
  bool commitSlot(Queued* slot, size_t len, uint32_t tag);
  bool transmit(const uint8_t* frame, size_t len, uint32_t tag, bool user, uint32_t nowMs);
  void pump(uint32_t nowMs);
  void sendStageCommand(uint32_t nowMs);
  void enterStage(Stage next);
  uint8_t channelSlots() const;

  void handlePush(const uint8_t* frame, size_t len);
  void handleResponse(const uint8_t* frame, size_t len, uint32_t nowMs);
  void finishInFlight(Outcome outcome, uint8_t errCode);
  void advanceHandshake(uint8_t cmd, Outcome outcome);
};

}  // namespace meshc
