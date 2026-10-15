#include "MeshService.h"

#if CROSSPOINT_MESH

#include <Arduino.h>
#include <HalPowerManager.h>
#include <Logging.h>
#include <Memory.h>
#include <TrustedTime.h>
#include <Utf8.h>

#include <cstdio>
#include <cstring>

#include "MeshRadioStore.h"

namespace {

constexpr uint32_t BATTERY_POLL_MS = 60000;
constexpr uint32_t RECONNECT_BASE_MS = 2000;
// 2, 4, 8, 16 and 32 s apart, then the user has to reconnect by hand. Without
// a limit, a radio that is switched off would keep this device awake.
constexpr uint8_t MAX_RECONNECT_ATTEMPTS = 5;
constexpr uint32_t STOP_WAIT_MS = 200;
constexpr uint32_t SLEEP_SHUTDOWN_BUDGET_MS = 2500;
// Long enough to cover a screen change, short enough that opening a book
// soon frees BLE's memory and power.
constexpr uint32_t RELEASE_GRACE_MS = 4000;
constexpr uint32_t HELD_START_RETRY_MS = 1000;
// BLE that fails this often will not come up; the next acquire() tries again.
constexpr uint8_t MAX_HELD_START_FAILURES = 5;

constexpr uint16_t MAX_UNREAD = 999;
constexpr size_t MIN_CONTACT_CAPACITY = 32;

bool reached(uint32_t nowMs, uint32_t deadlineMs) { return static_cast<int32_t>(nowMs - deadlineMs) >= 0; }

// Copies text from the mesh for storage and display. Control characters
// become spaces: the list rows do not lay out line breaks or tabs. out must
// hold len + 1 bytes.
void copySanitized(char* out, const char* text, size_t len) {
  for (size_t i = 0; i < len; i++) {
    const auto c = static_cast<unsigned char>(text[i]);
    out[i] = (c < 0x20 || c == 0x7F) ? ' ' : text[i];
  }
  out[len] = 0;
}

void copyText(meshc::StoredMessage& out, const char* text, size_t len) {
  if (len > meshc::MAX_STORED_TEXT_BYTES) len = meshc::MAX_STORED_TEXT_BYTES;
  copySanitized(out.text, text, len);
  out.textLen = static_cast<uint16_t>(len);
}

// snprintf cuts at a byte limit; drop a code point that was split there.
void trimToWholeCodePoints(char* text) { text[utf8SafeTruncateBuffer(text, static_cast<int>(strlen(text)))] = 0; }

// FNV-1a. Tells a repeated message from a new one; not a security check.
uint32_t textHash(const char* text, size_t len) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < len; i++) hash = (hash ^ static_cast<uint8_t>(text[i])) * 16777619u;
  return hash;
}

void formatPreview(char* out, size_t size, const meshc::StoredMessage& message) {
  if (message.sender[0] != 0) {
    snprintf(out, size, "%s: %s", message.sender, message.text);
  } else {
    snprintf(out, size, "%s", message.text);
  }
  trimToWholeCodePoints(out);
}

MeshService::Problem toProblem(MeshLinkError error) {
  switch (error) {
    case MeshLinkError::ConnectFailed:
      return MeshService::Problem::ConnectFailed;
    case MeshLinkError::PairingFailed:
      return MeshService::Problem::PairingFailed;
    case MeshLinkError::NotCompanion:
      return MeshService::Problem::NotCompanion;
    case MeshLinkError::LinkLost:
      return MeshService::Problem::LinkLost;
    case MeshLinkError::None:
      break;
  }
  return MeshService::Problem::None;
}

}  // namespace

MeshService& MeshService::getInstance() {
  static MeshService instance;
  return instance;
}

MeshService::MeshService() : client(&MeshService::sendFrame, this, *this) {}

bool MeshService::sendFrame(void*, const uint8_t* frame, size_t len) { return MESH_LINK.send(frame, len); }

bool MeshService::start() {
  if (MESH_LINK.isRunning()) return true;
  // The BLE controller must start and stop at normal CPU frequency. Only the
  // main loop lowers it, and this runs on the main loop, so it cannot drop
  // again before these calls return.
  powerManager.setPowerSaving(false);
  if (stopping) {
    if (!MESH_LINK.end(STOP_WAIT_MS)) return false;
    stopping = false;
  }
  if (!session) session = makeUniqueNoThrow<Session>();
  if (!session) {
    LOG_ERR("MESH", "OOM: session state");
    return false;
  }
  if (!MESH_LINK.begin("CrossPoint")) {
    session.reset();
    return false;
  }

  MESH_RADIO_STORE.loadFromFile();
  lastLinkState = MeshLinkState::Idle;
  lastProblem = Problem::None;
  handshakeFailed = false;
  autoReconnect = false;
  reconnectAttempts = 0;
  radioEpoch = 0;
  favoriteFailed = false;
  // Read now, so the list is ready when the radio's details are.
  meshhistory::forEachContactThread(&MeshService::addSavedConversation, this);
  // After the user's Disconnect, only the user connects again.
  if (MESH_RADIO_STORE.hasRadio() && !userDisconnected) connectSaved();
  touch();
  return true;
}

void MeshService::stop() {
  if (!MESH_LINK.isRunning()) return;
  endSession();
  autoReconnect = false;
  powerManager.setPowerSaving(false);
  stopping = !MESH_LINK.end(STOP_WAIT_MS);
  lastLinkState = MeshLinkState::Off;
  touch();
}

bool MeshService::acquire() {
  if (holders < UINT8_MAX) holders++;
  stopPending = false;
  heldStartFailures = 0;
  nextStartMs = millis() + HELD_START_RETRY_MS;
  return start();
}

void MeshService::release(const bool keepBriefly) {
  if (holders == 0) return;
  holders--;
  if (holders > 0) return;
  if (!keepBriefly) {
    stopPending = false;
    stop();
    return;
  }
  stopPending = true;
  stopAtMs = millis() + RELEASE_GRACE_MS;
}

void MeshService::stopIfReleased() {
  if (holders > 0) return;
  stopPending = false;
  stop();
}

void MeshService::shutdownForSleep() {
  if (!MESH_LINK.isRunning() && !stopping) return;
  endSession();
  autoReconnect = false;
  powerManager.setPowerSaving(false);
  const uint32_t startMs = millis();
  while (!MESH_LINK.end(500)) {
    if (millis() - startMs >= SLEEP_SHUTDOWN_BUDGET_MS) {
      LOG_ERR("MESH", "BLE still up at sleep; the radio will time the link out");
      break;
    }
  }
  stopping = MESH_LINK.isStopping();
  lastLinkState = MeshLinkState::Off;
}

void MeshService::endSession() {
  // A chat has always closed its thread by now; this keeps a missed one from
  // being called after its screen is gone.
  threadListener = nullptr;
  openThreadKey[0] = 0;
  // reset() fails the sends the radio has not taken, and the rest lose
  // their wait for an ack. Each is written to the history with that state,
  // so the session must outlive this.
  client.reset();
  settleAllSends();
  session.reset();
}

void MeshService::poll() {
  if (stopping) {
    if (MESH_LINK.end(0)) {
      stopping = false;
      touch();
    }
    return;
  }

  const uint32_t nowMs = millis();
  if (stopPending && holders == 0 && reached(nowMs, stopAtMs)) {
    stopIfReleased();
    return;
  }
  // start() fails while BLE is still coming down from an earlier stop.
  if (holders > 0 && !MESH_LINK.isRunning() && heldStartFailures < MAX_HELD_START_FAILURES &&
      reached(nowMs, nextStartMs)) {
    nextStartMs = nowMs + HELD_START_RETRY_MS;
    if (!start()) heldStartFailures++;
  }
  if (!MESH_LINK.isRunning()) return;

  const MeshLinkState state = MESH_LINK.state();
  if (state != lastLinkState) {
    onLinkStateChanged(lastLinkState, state, nowMs);
    lastLinkState = state;
    touch();
  }

  uint8_t frame[MeshLink::MAX_FRAME_SIZE];
  size_t len = 0;
  while (MESH_LINK.receive(frame, sizeof(frame), len)) client.onFrame(frame, len, nowMs);

  if (state == MeshLinkState::Ready) {
    client.poll(nowMs);
    if (handshakeFailed) {
      // Linked but the radio does not speak the protocol: stop, and leave
      // the next attempt to the user.
      handshakeFailed = false;
      autoReconnect = false;
      lastProblem = Problem::RadioSilent;
      MESH_LINK.disconnect();
    } else if (client.isReady() && client.isIdle() && reached(nowMs, nextBatteryPollMs)) {
      if (client.requestBattery()) nextBatteryPollMs = nowMs + BATTERY_POLL_MS;
    }
    if (session) {
      pollRequest(nowMs);
      uint32_t tag = 0;
      switch (session->tracker.nextTimeout(nowMs, tag)) {
        case meshc::DirectSendTracker::Timeout::None:
          break;
        case meshc::DirectSendTracker::Timeout::Retry:
          resendDirect(tag, false);
          break;
        case meshc::DirectSendTracker::Timeout::RetryFlood:
          resendDirect(tag, true);
          break;
        case meshc::DirectSendTracker::Timeout::GiveUp:
          if (PendingSend* pending = findPending(tag)) settleSend(*pending, meshc::Delivery::Unconfirmed);
          break;
      }
    }
  } else if (state == MeshLinkState::Idle && autoReconnect && reached(nowMs, reconnectAtMs)) {
    if (MESH_RADIO_STORE.hasRadio()) {
      MESH_LINK.connect(MESH_RADIO_STORE.radioAddr(), MESH_RADIO_STORE.radioAddrType());
    } else {
      autoReconnect = false;
    }
  }
}

void MeshService::onLinkStateChanged(MeshLinkState from, MeshLinkState to, uint32_t nowMs) {
  if (from == MeshLinkState::Ready) {
    client.reset();
    // No ack can arrive over a link that is gone.
    settleAllSends();
    // Nor the answer to a request; and a server may have dropped the login.
    finishRequest(false);
    if (session) session->loginCount = 0;
  }

  if (to == MeshLinkState::Ready) {
    lastProblem = Problem::None;
    reconnectAttempts = 0;
    radioMilliVolts = 0;
    // The radio reports its contacts and channels again during the handshake.
    if (session) {
      session->contactsListed = false;
      session->favoriteTag = 0;
      for (uint8_t i = 0; i < session->channelCount; i++) session->channels[i].live = false;
    }
    radioEpoch = 0;
    nextBatteryPollMs = nowMs;
    // A reconnect keeps the stored node name; only a different radio is saved here.
    if (strncmp(MESH_RADIO_STORE.radioAddr(), pendingDevice.addr, sizeof(pendingDevice.addr)) != 0) {
      MESH_RADIO_STORE.setRadio(pendingDevice.addr, pendingDevice.addrType, pendingDevice.name);
    }

    meshc::MeshCompanionClient::StartOptions options;
    // The radio only ever moves its clock forward, so a stale value here is
    // refused and harmless. 0 (no trusted time) leaves its clock alone.
    const int64_t trusted = trustedtime::trustedNow();
    options.setTimeEpoch = trusted > 0 ? static_cast<uint32_t>(trusted) : 0;
    requestedEpoch = options.setTimeEpoch;
    client.start(options, nowMs);
    return;
  }

  const bool attemptEnded = from == MeshLinkState::Connecting || from == MeshLinkState::NeedPin;
  if (to != MeshLinkState::Idle || (!attemptEnded && from != MeshLinkState::Ready)) return;

  const MeshLinkError error = MESH_LINK.lastError();
  // RadioSilent was set by poll() just before it dropped the link.
  if (lastProblem != Problem::RadioSilent) lastProblem = toProblem(error);
  if (error == MeshLinkError::ConnectFailed || error == MeshLinkError::LinkLost) {
    scheduleReconnect(nowMs);
  } else {
    // Pairing and protocol failures need the user: a new PIN or another radio.
    autoReconnect = false;
  }
}

void MeshService::scheduleReconnect(uint32_t nowMs) {
  if (!autoReconnect) return;
  if (reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
    autoReconnect = false;
    return;
  }
  reconnectAtMs = nowMs + (RECONNECT_BASE_MS << reconnectAttempts);
  reconnectAttempts++;
}

MeshService::Phase MeshService::phase() const {
  if (stopping) return Phase::Stopping;
  if (!MESH_LINK.isRunning()) return Phase::Off;
  switch (MESH_LINK.state()) {
    case MeshLinkState::Off:
      return Phase::Off;
    case MeshLinkState::Idle:
      return Phase::Idle;
    case MeshLinkState::Scanning:
      return Phase::Scanning;
    case MeshLinkState::Connecting:
      return Phase::Connecting;
    case MeshLinkState::NeedPin:
      return Phase::NeedPin;
    case MeshLinkState::Ready:
      return client.isReady() ? Phase::Ready : Phase::Handshake;
  }
  return Phase::Off;
}

bool MeshService::startScan() {
  autoReconnect = false;
  userDisconnected = false;
  lastProblem = Problem::None;
  const bool started = MESH_LINK.startScan();
  touch();
  return started;
}

bool MeshService::connectTo(const MeshLinkDevice& device) {
  pendingDevice = device;
  lastProblem = Problem::None;
  autoReconnect = true;
  userDisconnected = false;
  reconnectAttempts = 0;
  const bool started = MESH_LINK.connect(device.addr, device.addrType);
  touch();
  return started;
}

bool MeshService::connectSaved() {
  if (!MESH_RADIO_STORE.hasRadio()) return false;
  MeshLinkDevice device{};
  strncpy(device.addr, MESH_RADIO_STORE.radioAddr(), sizeof(device.addr) - 1);
  device.addrType = MESH_RADIO_STORE.radioAddrType();
  strncpy(device.name, MESH_RADIO_STORE.radioName(), sizeof(device.name) - 1);
  return connectTo(device);
}

bool MeshService::submitPin(uint32_t pin) {
  const bool accepted = MESH_LINK.submitPin(pin);
  touch();
  return accepted;
}

void MeshService::disconnect() {
  autoReconnect = false;
  userDisconnected = true;
  MESH_LINK.stopScan();
  MESH_LINK.disconnect();
  touch();
}

void MeshService::forgetSavedRadio() {
  if (!MESH_RADIO_STORE.hasRadio()) return;
  autoReconnect = false;
  MESH_LINK.forget(MESH_RADIO_STORE.radioAddr(), MESH_RADIO_STORE.radioAddrType());
  MESH_RADIO_STORE.clearRadio();
  touch();
}

// Zero-hop: heard by nodes in direct range only, which costs the mesh no
// repeater airtime.
bool MeshService::sendAdvert() { return client.sendSelfAdvert(false); }

void MeshService::onSelfInfo(const meshc::SelfInfo& info) {
  // Prefer the node's own name over the BLE name seen during the scan.
  if (info.name[0] != 0 && strcmp(info.name, MESH_RADIO_STORE.radioName()) != 0) {
    MESH_RADIO_STORE.setRadio(MESH_RADIO_STORE.hasRadio() ? MESH_RADIO_STORE.radioAddr() : pendingDevice.addr,
                              MESH_RADIO_STORE.hasRadio() ? MESH_RADIO_STORE.radioAddrType() : pendingDevice.addrType,
                              info.name);
  }
  // A settings change is done once the radio reports itself again.
  if (session && session->request == Request::Settings && session->requestStatus == RequestState::Busy &&
      session->requestTag == 0) {
    session->requestStatus = RequestState::Done;
  }
  touch();
}

void MeshService::onDeviceTime(uint32_t epochSecs) {
  // This arrives before the radio takes the time start() asked for, and the
  // radio only accepts a later time, so the later of the two is its clock.
  radioEpoch = epochSecs > requestedEpoch ? epochSecs : requestedEpoch;
  radioEpochAtMs = millis();
}

uint32_t MeshService::now() const {
  if (radioEpoch != 0) return radioEpoch + (millis() - radioEpochAtMs) / 1000;
  const int64_t trusted = trustedtime::trustedNow();
  return trusted > 0 ? static_cast<uint32_t>(trusted) : 0;
}

void MeshService::onReady() {
  // The handshake's bulk transfer is over.
  MESH_LINK.setFastConnection(false);
  dropStaleChannels();
  touch();
}

void MeshService::onHandshakeFailed(uint8_t cmd, meshc::FailReason) {
  LOG_ERR("MESH", "Radio did not answer command %u", static_cast<unsigned>(cmd));
  handshakeFailed = true;
}

void MeshService::onContactsBegin(uint32_t count) {
  if (!session || session->contactsListed) return;  // later lists only carry changes
  session->contactsListed = true;

  // One block for the session, 72 bytes a contact: 25 KB for a radio that
  // holds 350. It grows only when another radio holds more.
  // Old firmware does not report its limit: leave room for contacts it adds.
  size_t wanted = client.deviceInfo().maxContacts;
  if (wanted < count) wanted = count + MIN_CONTACT_CAPACITY;
  if (wanted < MIN_CONTACT_CAPACITY) wanted = MIN_CONTACT_CAPACITY;
  if (wanted > MAX_CONTACTS) wanted = MAX_CONTACTS;
  if (wanted > session->contacts.capacity()) {
    // Freed first, so the two blocks are never held together.
    session->contacts.setStorage(nullptr, 0);
    session->contactStorage.reset();
    session->contactStorage = makeUniqueNoThrow<meshc::ContactEntry[]>(wanted);
    if (!session->contactStorage) LOG_ERR("MESH", "OOM: %u contacts", static_cast<unsigned>(wanted));
    session->contacts.setStorage(session->contactStorage.get(), session->contactStorage ? wanted : 0);
  }
  session->contacts.clear();
  session->contactsFullLogged = false;
  contactsRev++;
  touch();
}

void MeshService::onContact(const meshc::Contact& contact) {
  if (!session) return;
  // Adverts refresh contacts all the time on a busy mesh. Only a change a
  // screen shows may ask it to repaint: contactsRev for the contact list,
  // touch() for the Mesh screen.
  bool onMeshScreen = false;
  switch (session->contacts.upsert(contact)) {
    case meshc::ContactBook::Upsert::Full:
      if (!session->contactsFullLogged) {
        LOG_ERR("MESH", "Contact table full at %u", static_cast<unsigned>(session->contacts.count()));
      }
      session->contactsFullLogged = true;
      return;
    case meshc::ContactBook::Upsert::Same:
      return;
    case meshc::ContactBook::Upsert::Added:
      onMeshScreen = true;  // the contact count
      break;
    case meshc::ContactBook::Upsert::Changed:
      break;
  }
  contactsRev++;
  if (Conversation* conversation = findConversation(contact.publicKey)) {
    if (strncmp(conversation->name, contact.name, sizeof(conversation->name)) != 0) {
      snprintf(conversation->name, sizeof(conversation->name), "%s", contact.name);
      onMeshScreen = true;
    }
  }
  if (onMeshScreen) touch();
}

void MeshService::onContactDeleted(const uint8_t* publicKey) {
  if (!session || !session->contacts.remove(publicKey)) return;
  contactsRev++;
  touch();
}

uint32_t MeshService::takeTag() {
  const uint32_t tag = nextTag;
  if (++nextTag == 0) nextTag = 1;
  return tag;
}

bool MeshService::setFavorite(const uint8_t* keyPrefix, bool favorite) {
  if (!session || session->favoriteTag != 0) return false;
  const meshc::ContactEntry* entry = session->contacts.findByPrefix(keyPrefix);
  if (!entry) return false;
  // Read the radio's whole record first: writing a contact replaces every
  // field, and this side keeps only some of them.
  const uint32_t tag = takeTag();
  if (!client.requestContact(entry->publicKey, tag)) return false;
  session->favoriteTag = tag;
  session->favoriteWanted = favorite;
  // Kept to check the answer against: the mark must never land on another contact.
  memcpy(session->favoriteContact.publicKey, entry->publicKey, meshc::PUB_KEY_SIZE);
  return true;
}

bool MeshService::favoriteBusy() const { return session && session->favoriteTag != 0; }

void MeshService::onContactFetched(uint32_t tag, const meshc::Contact& contact) {
  if (requestContactFetched(tag, contact)) return;
  if (!session || tag == 0 || tag != session->favoriteTag) return;
  if (memcmp(contact.publicKey, session->favoriteContact.publicKey, meshc::PUB_KEY_SIZE) == 0) {
    session->favoriteContact = contact;
    uint8_t& flags = session->favoriteContact.flags;
    flags = static_cast<uint8_t>(session->favoriteWanted ? (flags | meshc::CONTACT_FLAG_FAVORITE)
                                                         : (flags & ~meshc::CONTACT_FLAG_FAVORITE));
    if (client.updateContact(session->favoriteContact, tag)) return;
  }
  session->favoriteTag = 0;
  favoriteFailed = true;
}

void MeshService::onCommandDone(uint8_t cmd, uint32_t tag) {
  if (requestCommandDone(cmd, tag)) return;
  if (!session || tag == 0 || tag != session->favoriteTag || cmd != meshc::cmd::ADD_UPDATE_CONTACT) return;
  session->favoriteTag = 0;
  // Gone from this side's table: the radio dropped the contact meanwhile and
  // the write added it back. Read the changes to pick it up again.
  if (!session->contacts.setFlags(session->favoriteContact.publicKey, session->favoriteContact.flags)) {
    client.requestContactsRefresh();
  }
  contactsRev++;
}

bool MeshService::takeFavoriteFailure() {
  const bool failed = favoriteFailed;
  favoriteFailed = false;
  return failed;
}

size_t MeshService::contactCount() const { return session ? session->contacts.count() : 0; }

size_t MeshService::contactCapacity() const { return session ? session->contacts.capacity() : 0; }

bool MeshService::contact(size_t index, meshc::ContactEntry& out) const {
  if (!session || index >= session->contacts.count()) return false;
  out = session->contacts.at(index);
  return true;
}

bool MeshService::findContact(const uint8_t* keyPrefix, meshc::ContactEntry& out) const {
  if (!session) return false;
  const meshc::ContactEntry* entry = session->contacts.findByPrefix(keyPrefix);
  if (!entry) return false;
  out = *entry;
  return true;
}

uint8_t MeshService::conversationCount() const { return session ? session->conversationCount : 0; }

bool MeshService::conversation(uint8_t position, ConversationView& out) const {
  if (!session || position >= session->conversationCount) return false;
  const Conversation& entry = session->conversations[position];
  out.name = entry.name;
  out.threadKey = entry.threadKey;
  out.preview = entry.preview;
  out.unread = entry.unread;
  return true;
}

MeshService::Conversation* MeshService::findConversation(const uint8_t* keyPrefix) {
  if (!session) return nullptr;
  for (uint8_t i = 0; i < session->conversationCount; i++) {
    if (memcmp(session->conversations[i].keyPrefix, keyPrefix, meshc::KEY_PREFIX_SIZE) == 0) {
      return &session->conversations[i];
    }
  }
  return nullptr;
}

MeshService::Conversation* MeshService::touchConversation(const uint8_t* keyPrefix) {
  if (!session) return nullptr;
  Conversation* list = session->conversations;
  const Conversation* found = findConversation(keyPrefix);
  uint8_t position = 0;
  if (found) {
    position = static_cast<uint8_t>(found - list);
  } else {
    // New: it takes the last place, which a full list gives up.
    if (session->conversationCount < MAX_CONVERSATIONS) session->conversationCount++;
    position = static_cast<uint8_t>(session->conversationCount - 1);
    Conversation& added = list[position];
    added = Conversation{};
    memcpy(added.keyPrefix, keyPrefix, meshc::KEY_PREFIX_SIZE);
    meshhistory::contactThreadKey(keyPrefix, added.threadKey);
    const meshc::ContactEntry* known = session->contacts.findByPrefix(keyPrefix);
    // The key in hex until the radio lists the contact.
    snprintf(added.name, sizeof(added.name), "%s", known ? known->name : added.threadKey + 2);
  }
  if (position != 0) {
    const Conversation moved = list[position];
    memmove(&list[1], &list[0], position * sizeof(Conversation));
    list[0] = moved;
  }
  return &list[0];
}

// forEachContactThread callback: one direct-message log found on the card.
// Keeps the MAX_CONVERSATIONS with the newest last message, newest first.
void MeshService::addSavedConversation(void* ctx, const char* threadKey) {
  auto* self = static_cast<MeshService*>(ctx);
  Session* session = self->session.get();
  uint8_t keyPrefix[meshc::KEY_PREFIX_SIZE];
  if (!session || !meshhistory::parseContactThreadKey(threadKey, keyPrefix)) return;
  const meshc::StoredMessage& last = session->scratch;
  if (meshhistory::loadTail(threadKey, &session->scratch, 1) != 1) return;

  Conversation* list = session->conversations;
  uint8_t position = 0;
  while (position < session->conversationCount && list[position].lastActivity >= last.timestamp) position++;
  if (position >= MAX_CONVERSATIONS) return;
  if (session->conversationCount < MAX_CONVERSATIONS) session->conversationCount++;
  memmove(&list[position + 1], &list[position], (session->conversationCount - 1 - position) * sizeof(Conversation));

  Conversation& added = list[position];
  added = Conversation{};
  memcpy(added.keyPrefix, keyPrefix, meshc::KEY_PREFIX_SIZE);
  snprintf(added.threadKey, sizeof(added.threadKey), "%s", threadKey);
  snprintf(added.name, sizeof(added.name), "%s", threadKey + 2);
  added.lastActivity = last.timestamp;
  if (!last.outgoing) markSeen(added, last.timestamp, textHash(last.text, last.textLen));
  formatPreview(added.preview, sizeof(added.preview), last);
}

void MeshService::onBattery(const meshc::BatteryInfo& info) {
  radioMilliVolts = info.milliVolts;
  touch();
}

uint8_t MeshService::channelCount() const { return session ? session->channelCount : 0; }

bool MeshService::channel(uint8_t position, ChannelView& out) const {
  if (!session || position >= session->channelCount) return false;
  const ChannelEntry& entry = session->channels[position];
  out.name = entry.name;
  out.threadKey = entry.threadKey;
  out.preview = entry.preview;
  out.unread = entry.unread;
  out.slot = entry.slot;
  out.secret = entry.secret;
  return true;
}

MeshService::ChannelEntry* MeshService::findChannelBySlot(uint8_t slot) {
  if (!session) return nullptr;
  for (uint8_t i = 0; i < session->channelCount; i++) {
    // A stale entry may carry the slot of a channel that has since moved.
    if (session->channels[i].live && session->channels[i].slot == slot) return &session->channels[i];
  }
  return nullptr;
}

MeshService::ChannelEntry* MeshService::findChannelByKey(const char* threadKey) {
  if (!session) return nullptr;
  for (uint8_t i = 0; i < session->channelCount; i++) {
    if (strcmp(session->channels[i].threadKey, threadKey) == 0) return &session->channels[i];
  }
  return nullptr;
}

MeshService::ChannelEntry* MeshService::claimChannelEntry(const char* threadKey) {
  if (!session) return nullptr;
  for (uint8_t i = 0; i < session->channelCount; i++) {
    ChannelEntry& kept = session->channels[i];
    if (!kept.live && strcmp(kept.threadKey, threadKey) == 0) return &kept;
  }
  ChannelEntry* added = nullptr;
  if (session->channelCount < MAX_CHANNELS) {
    added = &session->channels[session->channelCount++];
  } else {
    // Full of entries from the last link: take one the radio has not
    // reported again. If its channel still exists, it gets a new entry later
    // only when one is free.
    for (uint8_t i = 0; i < session->channelCount && !added; i++) {
      if (!session->channels[i].live) added = &session->channels[i];
    }
  }
  if (!added) return nullptr;
  *added = ChannelEntry{};
  snprintf(added->threadKey, sizeof(added->threadKey), "%s", threadKey);
  return added;
}

void MeshService::dropStaleChannels() {
  if (!session) return;
  uint8_t kept = 0;
  for (uint8_t i = 0; i < session->channelCount; i++) {
    if (!session->channels[i].live) continue;
    if (kept != i) session->channels[kept] = session->channels[i];
    kept++;
  }
  session->channelCount = kept;
}

void MeshService::onChannel(const meshc::Channel& channel) {
  char threadKey[meshhistory::THREAD_KEY_SIZE];
  meshhistory::channelThreadKey(channel.secret, threadKey);
  ChannelEntry* entry = claimChannelEntry(threadKey);
  if (!entry) {
    LOG_ERR("MESH", "Channel list full: slot %u not listed", static_cast<unsigned>(channel.index));
    return;
  }
  entry->slot = channel.index;
  entry->live = true;
  snprintf(entry->name, sizeof(entry->name), "%s", channel.name);
  memcpy(entry->secret, channel.secret, sizeof(entry->secret));
  if (entry->preview[0] == 0 && meshhistory::loadTail(threadKey, &session->scratch, 1) == 1) {
    formatPreview(entry->preview, sizeof(entry->preview), session->scratch);
  }
  requestChannelRead(channel.index, true);
  touch();
}

void MeshService::openThread(const char* threadKey, ThreadListener& listener) {
  strncpy(openThreadKey, threadKey, sizeof(openThreadKey) - 1);
  openThreadKey[sizeof(openThreadKey) - 1] = 0;
  threadListener = &listener;
  if (ChannelEntry* entry = findChannelByKey(threadKey)) entry->unread = 0;
  uint8_t keyPrefix[meshc::KEY_PREFIX_SIZE];
  if (meshhistory::parseContactThreadKey(threadKey, keyPrefix)) {
    if (Conversation* conversation = findConversation(keyPrefix)) conversation->unread = 0;
  }
  touch();
}

void MeshService::closeThread(const ThreadListener& listener) {
  if (threadListener != &listener) return;
  threadListener = nullptr;
  openThreadKey[0] = 0;
}

bool MeshService::isOpenThread(const char* threadKey) const {
  return threadListener != nullptr && strcmp(openThreadKey, threadKey) == 0;
}

void MeshService::onMessage(const meshc::Message& message) {
  if (!session) return;
  if (message.isChannel) {
    handleChannelMessage(message);
  } else {
    handleDirectMessage(message);
  }
}

void MeshService::handleChannelMessage(const meshc::Message& message) {
  ChannelEntry* entry = findChannelBySlot(message.channelIndex);
  if (!entry) {
    // The handshake listed no channel for this slot (the radio does not
    // report a channel without a name). The radio has already removed the
    // message from its queue, so keep it under a name made from the slot.
    char threadKey[meshhistory::THREAD_KEY_SIZE];
    snprintf(threadKey, sizeof(threadKey), "s_%02x", static_cast<unsigned>(message.channelIndex));
    entry = claimChannelEntry(threadKey);
    if (!entry) {
      LOG_ERR("MESH", "Dropped a message for channel slot %u", static_cast<unsigned>(message.channelIndex));
      return;
    }
    LOG_ERR("MESH", "Message for unlisted channel slot %u kept as %s", static_cast<unsigned>(message.channelIndex),
            threadKey);
    entry->slot = message.channelIndex;
    entry->live = true;
    snprintf(entry->name, sizeof(entry->name), "#%u", static_cast<unsigned>(message.channelIndex));
  }

  meshc::StoredMessage& stored = session->scratch;
  stored = meshc::StoredMessage{};
  stored.timestamp = message.timestamp;
  stored.hasSnr = message.hasSnr;
  stored.snrQuarterDb = message.snrQuarterDb;
  stored.pathLen = message.pathLen;

  // The radio delivers "<sender>: <text>". Text without that form, or with a
  // name too long to be one, is kept whole.
  const char* body = message.text;
  size_t bodyLen = message.textLen;
  size_t nameLen = 0;
  size_t bodyOffset = 0;
  if (meshc::splitChannelText(message.text, message.textLen, nameLen, bodyOffset) &&
      nameLen <= meshc::MAX_SENDER_BYTES) {
    copySanitized(stored.sender, message.text, nameLen);
    body += bodyOffset;
    bodyLen -= bodyOffset;
  }
  copyText(stored, body, bodyLen);
  storeMessage(entry->threadKey, stored);
}

void MeshService::handleDirectMessage(const meshc::Message& message) {
  // Replies to commands are not chat text. Signed messages are: the parser
  // has already removed their signer prefix.
  if (message.textType != meshc::txt::PLAIN && message.textType != meshc::txt::SIGNED_PLAIN) return;

  meshc::StoredMessage& stored = session->scratch;
  stored = meshc::StoredMessage{};
  stored.timestamp = message.timestamp;
  stored.hasSnr = message.hasSnr;
  stored.snrQuarterDb = message.snrQuarterDb;
  stored.pathLen = message.pathLen;
  copyText(stored, message.text, message.textLen);
  if (message.textType == meshc::txt::SIGNED_PLAIN) {
    // A room server's post: the author is named by the start of their key.
    const meshc::ContactEntry* author = session->contacts.findByShortPrefix(message.signerPrefix);
    if (author) {
      snprintf(stored.sender, sizeof(stored.sender), "%s", author->name);
    } else {
      snprintf(stored.sender, sizeof(stored.sender), "%02x%02x%02x%02x", message.signerPrefix[0],
               message.signerPrefix[1], message.signerPrefix[2], message.signerPrefix[3]);
    }
  }

  const uint32_t hash = textHash(stored.text, stored.textLen);
  if (const Conversation* known = findConversation(message.senderPrefix)) {
    if (seenBefore(*known, stored.timestamp, hash)) return;  // a resend
  }
  Conversation* conversation = touchConversation(message.senderPrefix);
  markSeen(*conversation, stored.timestamp, hash);
  storeMessage(conversation->threadKey, stored);
}

bool MeshService::seenBefore(const Conversation& conversation, uint32_t timestamp, uint32_t hash) {
  for (uint8_t i = 0; i < SEEN_MESSAGES; i++) {
    // A hash of 0 with a timestamp of 0 is an empty place, not a message.
    if ((timestamp != 0 || hash != 0) && conversation.seenTimestamps[i] == timestamp &&
        conversation.seenHashes[i] == hash) {
      return true;
    }
  }
  return false;
}

void MeshService::markSeen(Conversation& conversation, uint32_t timestamp, uint32_t hash) {
  conversation.seenTimestamps[conversation.seenNext] = timestamp;
  conversation.seenHashes[conversation.seenNext] = hash;
  conversation.seenNext = static_cast<uint8_t>((conversation.seenNext + 1) % SEEN_MESSAGES);
}

void MeshService::noteMessage(const char* threadKey, const meshc::StoredMessage& message) {
  uint8_t keyPrefix[meshc::KEY_PREFIX_SIZE];
  if (meshhistory::parseContactThreadKey(threadKey, keyPrefix)) {
    if (Conversation* conversation = touchConversation(keyPrefix)) {
      conversation->lastActivity = now();
      formatPreview(conversation->preview, sizeof(conversation->preview), message);
    }
  } else if (ChannelEntry* entry = findChannelByKey(threadKey)) {
    formatPreview(entry->preview, sizeof(entry->preview), message);
  }
  touch();
}

void MeshService::storeMessage(const char* threadKey, const meshc::StoredMessage& message) {
  // A failed write is logged there; the message still shows for this session.
  meshhistory::append(threadKey, message);
  noteMessage(threadKey, message);

  if (isOpenThread(threadKey)) {
    threadListener->onThreadMessage(message, 0);
    return;
  }
  if (message.outgoing) return;
  uint8_t keyPrefix[meshc::KEY_PREFIX_SIZE];
  if (meshhistory::parseContactThreadKey(threadKey, keyPrefix)) {
    Conversation* conversation = findConversation(keyPrefix);
    if (!conversation) return;
    if (conversation->unread < MAX_UNREAD) conversation->unread++;
    conversation->unreadSeq = session->nextUnreadSeq++;
  } else if (ChannelEntry* entry = findChannelByKey(threadKey)) {
    if (entry->unread < MAX_UNREAD) entry->unread++;
    entry->unreadSeq = session->nextUnreadSeq++;
  }
}

uint16_t MeshService::unreadTotal() const {
  if (!session) return 0;
  uint32_t total = 0;
  for (uint8_t i = 0; i < session->channelCount; i++) total += session->channels[i].unread;
  for (uint8_t i = 0; i < session->conversationCount; i++) total += session->conversations[i].unread;
  return static_cast<uint16_t>(total < MAX_UNREAD ? total : MAX_UNREAD);
}

bool MeshService::latestUnread(UnreadView& out) const { return recentUnread(&out, 1) == 1; }

uint8_t MeshService::recentUnread(UnreadView* out, const uint8_t max) const {
  if (!session || out == nullptr || max == 0) return 0;
  // Insertion by unreadSeq, newest first; at most a few entries are kept.
  uint32_t seqs[8];
  const uint8_t limit = max < 8 ? max : 8;
  uint8_t count = 0;
  const auto offer = [&](const char* name, const char* preview, uint16_t unread, uint32_t seq) {
    if (unread == 0) return;
    uint8_t at = count;
    while (at > 0 && seqs[at - 1] < seq) at--;
    if (at >= limit) return;
    const uint8_t last = count < limit ? count : static_cast<uint8_t>(limit - 1);
    for (uint8_t k = last; k > at; k--) {
      out[k] = out[k - 1];
      seqs[k] = seqs[k - 1];
    }
    out[at].name = name;
    out[at].preview = preview;
    out[at].unread = unread;
    seqs[at] = seq;
    if (count < limit) count++;
  };
  for (uint8_t i = 0; i < session->conversationCount; i++) {
    const Conversation& entry = session->conversations[i];
    offer(entry.name, entry.preview, entry.unread, entry.unreadSeq);
  }
  for (uint8_t i = 0; i < session->channelCount; i++) {
    const ChannelEntry& entry = session->channels[i];
    offer(entry.name, entry.preview, entry.unread, entry.unreadSeq);
  }
  return count;
}

void MeshService::markRead(const char* threadKey) {
  if (ChannelEntry* entry = findChannelByKey(threadKey)) entry->unread = 0;
  uint8_t keyPrefix[meshc::KEY_PREFIX_SIZE];
  if (meshhistory::parseContactThreadKey(threadKey, keyPrefix)) {
    if (Conversation* conversation = findConversation(keyPrefix)) conversation->unread = 0;
  }
  touch();
}

void MeshService::markAllRead() {
  if (!session) return;
  for (uint8_t i = 0; i < session->channelCount; i++) session->channels[i].unread = 0;
  for (uint8_t i = 0; i < session->conversationCount; i++) session->conversations[i].unread = 0;
  touch();
}

bool MeshService::clearHistory(const char* threadKey) {
  if (!meshhistory::removeThread(threadKey)) return false;
  // A send still waiting for its ack must not patch whatever record lands at
  // its old offset in the new file: it is written again when it settles.
  if (session) {
    for (PendingSend& pending : session->pending) {
      if (pending.tag != 0 && strcmp(pending.threadKey, threadKey) == 0) pending.recordOffset = 0;
    }
  }
  if (ChannelEntry* entry = findChannelByKey(threadKey)) {
    entry->preview[0] = 0;
    entry->unread = 0;
  }
  uint8_t keyPrefix[meshc::KEY_PREFIX_SIZE];
  if (session && meshhistory::parseContactThreadKey(threadKey, keyPrefix)) {
    // A conversation is listed for its history; with none left it goes.
    if (Conversation* conversation = findConversation(keyPrefix)) {
      const auto position = static_cast<uint8_t>(conversation - session->conversations);
      memmove(&session->conversations[position], &session->conversations[position + 1],
              (session->conversationCount - 1 - position) * sizeof(Conversation));
      session->conversationCount--;
    }
  }
  touch();
  return true;
}

size_t MeshService::maxTextBytes(const char* threadKey) const {
  uint8_t keyPrefix[meshc::KEY_PREFIX_SIZE];
  if (meshhistory::parseContactThreadKey(threadKey, keyPrefix)) return meshc::MAX_TEXT_BYTES;
  return meshc::maxChannelTextBytes(client.selfInfo().name);
}

MeshService::PendingSend* MeshService::findPending(uint32_t tag) {
  if (!session || tag == 0) return nullptr;
  for (PendingSend& pending : session->pending) {
    if (pending.tag == tag) return &pending;
  }
  return nullptr;
}

bool MeshService::sendText(const char* threadKey, const char* text, size_t textLen) {
  if (!session || text == nullptr || textLen == 0 || textLen > maxTextBytes(threadKey)) return false;
  uint8_t keyPrefix[meshc::KEY_PREFIX_SIZE];
  const bool direct = meshhistory::parseContactThreadKey(threadKey, keyPrefix);

  meshc::StoredMessage& message = session->scratch;
  message = meshc::StoredMessage{};
  message.timestamp = now();
  message.outgoing = true;
  message.delivery = meshc::Delivery::Sending;
  // Channel messages carry the sender's name on the air; direct ones do not.
  if (!direct) snprintf(message.sender, sizeof(message.sender), "%s", client.selfInfo().name);
  copyText(message, text, textLen);

  PendingSend* pending = nullptr;
  for (PendingSend& candidate : session->pending) {
    if (candidate.tag == 0) {
      pending = &candidate;
      break;
    }
  }
  bool queued = false;
  if (pending) {
    // Recorded before the client sees it, so a reply cannot arrive first.
    pending->tag = nextTag;
    pending->accepted = false;
    pending->recordOffset = 0;
    snprintf(pending->threadKey, sizeof(pending->threadKey), "%s", threadKey);
    pending->message = message;
    if (direct) {
      queued = client.sendDirect(keyPrefix, message.text, message.textLen, message.timestamp, 0, nextTag);
    } else {
      const ChannelEntry* entry = findChannelByKey(threadKey);
      // A stale entry's slot may belong to another channel by now.
      queued = entry && entry->live &&
               client.sendChannel(entry->slot, message.text, message.textLen, message.timestamp, nextTag);
    }
    if (!queued) pending->tag = 0;
  }
  if (!queued) {
    // No link, no such channel on this radio, or the queue is full.
    message.delivery = meshc::Delivery::Failed;
    storeMessage(threadKey, message);
    return true;
  }

  // Without a tracker slot the message is sent once and never confirmed.
  if (direct) session->tracker.begin(nextTag);
  if (isOpenThread(threadKey)) threadListener->onThreadMessage(message, nextTag);
  takeTag();
  touch();
  return true;
}

void MeshService::settleSend(PendingSend& pending, meshc::Delivery delivery) {
  const uint32_t tag = pending.tag;
  const bool changed = pending.message.delivery != delivery;
  pending.message.delivery = delivery;
  if (pending.recordOffset == 0) {
    meshhistory::append(pending.threadKey, pending.message);
    noteMessage(pending.threadKey, pending.message);
  } else if (changed) {
    // Already in the history and in its row. Newer messages may have
    // followed it there, so the row's preview is left alone.
    meshhistory::setDelivery(pending.threadKey, pending.recordOffset, delivery);
  }
  session->tracker.end(tag);
  if (isOpenThread(pending.threadKey)) threadListener->onOutgoingState(tag, pending.message, true);
  pending.tag = 0;
}

void MeshService::settleAllSends() {
  if (!session) return;
  for (PendingSend& pending : session->pending) {
    if (pending.tag == 0) continue;
    // The radio took it; only the ack is missing.
    settleSend(pending, pending.accepted ? meshc::Delivery::Unconfirmed : meshc::Delivery::Failed);
  }
}

// Channel messages get no delivery receipt: accepted by the radio is the last
// state they reach.
void MeshService::onChannelAccepted(uint32_t tag) {
  if (PendingSend* pending = findPending(tag)) settleSend(*pending, meshc::Delivery::Sent);
}

void MeshService::onDirectAccepted(uint32_t tag, const meshc::SentInfo& info) {
  PendingSend* pending = findPending(tag);
  if (!pending) return;
  if (!pending->accepted) {
    // First try: the message goes into the history now, so it is not lost if
    // the device sleeps before the ack. Later states change that record.
    pending->accepted = true;
    pending->message.delivery = meshc::Delivery::Sent;
    pending->recordOffset = meshhistory::append(pending->threadKey, pending->message);
    noteMessage(pending->threadKey, pending->message);
  }
  if (!session->tracker.onAccepted(tag, info, millis())) {
    settleSend(*pending, meshc::Delivery::Sent);
    return;
  }
  if (isOpenThread(pending->threadKey)) threadListener->onOutgoingState(tag, pending->message, false);
}

void MeshService::onDeliveryConfirmed(uint32_t ackCode, uint32_t) {
  if (!session) return;
  if (PendingSend* pending = findPending(session->tracker.onConfirmed(ackCode))) {
    settleSend(*pending, meshc::Delivery::Delivered);
  }
}

void MeshService::resendDirect(uint32_t tag, bool clearRoute) {
  PendingSend* pending = findPending(tag);
  uint8_t keyPrefix[meshc::KEY_PREFIX_SIZE];
  if (!pending || !meshhistory::parseContactThreadKey(pending->threadKey, keyPrefix)) {
    session->tracker.end(tag);
    return;
  }
  if (clearRoute) {
    // Queued ahead of the message, so the radio floods this try.
    const meshc::ContactEntry* contact = session->contacts.findByPrefix(keyPrefix);
    if (contact) client.resetPath(contact->publicKey);
  }
  // The same timestamp marks it as the same message for the recipient.
  const meshc::StoredMessage& message = pending->message;
  if (!client.sendDirect(keyPrefix, message.text, message.textLen, message.timestamp, session->tracker.attempt(tag),
                         tag)) {
    settleSend(*pending, meshc::Delivery::Unconfirmed);
  }
}

void MeshService::onCommandFailed(uint8_t cmd, uint32_t tag, meshc::FailReason reason, uint8_t errCode) {
  LOG_ERR("MESH", "Command %u failed: reason %u, radio error %u", static_cast<unsigned>(cmd),
          static_cast<unsigned>(reason), static_cast<unsigned>(errCode));
  if (requestCommandFailed(tag)) return;
  if (session && tag != 0 && tag == session->favoriteTag) {
    session->favoriteTag = 0;
    favoriteFailed = true;
    return;
  }
  PendingSend* pending = findPending(tag);
  if (!pending) return;
  // After an accepted try this was a retry. A radio that fell silent may
  // have sent the message all the same.
  const bool maybeSent = pending->accepted || reason == meshc::FailReason::Timeout;
  settleSend(*pending, maybeSent ? meshc::Delivery::Unconfirmed : meshc::Delivery::Failed);
}

#endif  // CROSSPOINT_MESH
