#include "MeshCompanionClient.h"

#include <cstring>

namespace meshc {

namespace {

// Wrap-safe "now is at or past deadline" for a free-running millisecond clock.
bool reached(uint32_t nowMs, uint32_t deadlineMs) { return static_cast<int32_t>(nowMs - deadlineMs) >= 0; }

FailReason toFailReason(bool timedOut) { return timedOut ? FailReason::Timeout : FailReason::RadioError; }

}  // namespace

MeshCompanionClient::MeshCompanionClient(SendFn send, void* sendCtx, MeshEvents& events)
    : send(send), sendCtx(sendCtx), events(events) {}

void MeshCompanionClient::start(const StartOptions& options, uint32_t nowMs) {
  reset();
  contactsSince = options.contactsSince;
  setTimeEpoch = options.setTimeEpoch;
  startMs = nowMs;
  strncpy(appName, options.appName ? options.appName : "", sizeof(appName) - 1);
  appName[sizeof(appName) - 1] = 0;
  enterStage(Stage::DeviceQuery);
  pump(nowMs);
}

void MeshCompanionClient::reset() {
  if (inFlight && inFlightUser) events.onCommandFailed(inFlightCmd, inFlightTag, FailReason::LinkDown, 0);
  inFlight = false;
  while (queueCount > 0) {
    const Queued& item = queue[queueHead];
    events.onCommandFailed(item.frame[0], item.tag, FailReason::LinkDown, 0);
    queueHead = (queueHead + 1) % QUEUE_CAPACITY;
    queueCount--;
  }
  queueHead = 0;
  currentStage = Stage::Idle;
  stageRetries = 0;
  channelCursor = 0;
  syncGotMessage = false;
  syncWanted = false;
  contactsDirty = false;
  radioInfo = DeviceInfo{};
  nodeInfo = SelfInfo{};
}

void MeshCompanionClient::onFrame(const uint8_t* frame, size_t len, uint32_t nowMs) {
  if (frame == nullptr || len == 0) return;
  if (frame[0] >= FIRST_PUSH_CODE) {
    handlePush(frame, len);
  } else if (inFlight) {
    handleResponse(frame, len, nowMs);
  }
  pump(nowMs);
}

void MeshCompanionClient::poll(uint32_t nowMs) {
  if (inFlight && reached(nowMs, deadlineMs)) finishInFlight(Outcome::Timeout, 0);
  pump(nowMs);
}

MeshCompanionClient::Queued* MeshCompanionClient::reserveSlot() {
  if (!isReady() || queueCount >= QUEUE_CAPACITY) return nullptr;
  return &queue[(queueHead + queueCount) % QUEUE_CAPACITY];
}

bool MeshCompanionClient::commitSlot(Queued* slot, size_t len, uint32_t tag) {
  if (len == 0) return false;
  slot->len = static_cast<uint8_t>(len);
  slot->tag = tag;
  queueCount++;
  return true;
}

bool MeshCompanionClient::sendDirect(const uint8_t* keyPrefix, const char* text, size_t textLen, uint32_t timestamp,
                                     uint8_t attempt, uint32_t tag) {
  Queued* slot = reserveSlot();
  if (!slot) return false;
  return commitSlot(slot, buildSendTxtMsg(slot->frame, MAX_FRAME_SIZE, attempt, timestamp, keyPrefix, text, textLen),
                    tag);
}

bool MeshCompanionClient::sendChannel(uint8_t channelIndex, const char* text, size_t textLen, uint32_t timestamp,
                                      uint32_t tag) {
  if (textLen > maxChannelTextBytes(nodeInfo.name)) return false;
  Queued* slot = reserveSlot();
  if (!slot) return false;
  return commitSlot(slot, buildSendChannelTxtMsg(slot->frame, MAX_FRAME_SIZE, channelIndex, timestamp, text, textLen),
                    tag);
}

bool MeshCompanionClient::requestBattery() {
  Queued* slot = reserveSlot();
  return slot && commitSlot(slot, buildGetBattAndStorage(slot->frame, MAX_FRAME_SIZE), 0);
}

bool MeshCompanionClient::sendSelfAdvert(bool flood) {
  Queued* slot = reserveSlot();
  return slot && commitSlot(slot, buildSendSelfAdvert(slot->frame, MAX_FRAME_SIZE, flood), 0);
}

bool MeshCompanionClient::setAdvertName(const char* name, uint32_t tag) {
  Queued* slot = reserveSlot();
  return slot && commitSlot(slot, buildSetAdvertName(slot->frame, MAX_FRAME_SIZE, name), tag);
}

bool MeshCompanionClient::setTxPower(int8_t dbm, uint32_t tag) {
  Queued* slot = reserveSlot();
  return slot && commitSlot(slot, buildSetRadioTxPower(slot->frame, MAX_FRAME_SIZE, dbm), tag);
}

bool MeshCompanionClient::removeContact(const uint8_t* publicKey) {
  Queued* slot = reserveSlot();
  return slot && commitSlot(slot, buildRemoveContact(slot->frame, MAX_FRAME_SIZE, publicKey), 0);
}

bool MeshCompanionClient::requestContact(const uint8_t* publicKey, uint32_t tag) {
  Queued* slot = reserveSlot();
  return slot && commitSlot(slot, buildGetContactByKey(slot->frame, MAX_FRAME_SIZE, publicKey), tag);
}

bool MeshCompanionClient::updateContact(const Contact& contact, uint32_t tag) {
  Queued* slot = reserveSlot();
  return slot && commitSlot(slot, buildAddUpdateContact(slot->frame, MAX_FRAME_SIZE, contact), tag);
}

bool MeshCompanionClient::resetPath(const uint8_t* publicKey) {
  Queued* slot = reserveSlot();
  return slot && commitSlot(slot, buildResetPath(slot->frame, MAX_FRAME_SIZE, publicKey), 0);
}

bool MeshCompanionClient::setRadioParams(uint32_t freqKhz, uint32_t bwHz, uint8_t spreadingFactor, uint8_t codingRate,
                                         uint32_t tag) {
  Queued* slot = reserveSlot();
  return slot &&
         commitSlot(slot,
                    buildSetRadioParams(slot->frame, MAX_FRAME_SIZE, freqKhz, bwHz, spreadingFactor, codingRate,
                                        radioInfo.hasClientRepeat ? static_cast<int8_t>(radioInfo.clientRepeat) : -1),
                    tag);
}

bool MeshCompanionClient::setAdvertPosition(int32_t latE6, int32_t lonE6, uint32_t tag) {
  Queued* slot = reserveSlot();
  return slot && commitSlot(slot, buildSetAdvertLatLon(slot->frame, MAX_FRAME_SIZE, latE6, lonE6), tag);
}

bool MeshCompanionClient::setOtherParams(const SelfInfo& self, uint32_t tag) {
  Queued* slot = reserveSlot();
  return slot && commitSlot(slot, buildSetOtherParams(slot->frame, MAX_FRAME_SIZE, self), tag);
}

bool MeshCompanionClient::refreshSelfInfo() {
  Queued* slot = reserveSlot();
  return slot && commitSlot(slot, buildAppStart(slot->frame, MAX_FRAME_SIZE, appName), 0);
}

bool MeshCompanionClient::setChannel(const Channel& channel, uint32_t tag) {
  Queued* slot = reserveSlot();
  return slot && commitSlot(slot, buildSetChannel(slot->frame, MAX_FRAME_SIZE, channel), tag);
}

bool MeshCompanionClient::readChannel(uint8_t index) {
  Queued* slot = reserveSlot();
  return slot && commitSlot(slot, buildGetChannel(slot->frame, MAX_FRAME_SIZE, index), 0);
}

bool MeshCompanionClient::login(const uint8_t* publicKey, const char* password, uint32_t tag) {
  Queued* slot = reserveSlot();
  return slot && commitSlot(slot, buildSendLogin(slot->frame, MAX_FRAME_SIZE, publicKey, password), tag);
}

bool MeshCompanionClient::requestStatus(const uint8_t* publicKey, uint32_t tag) {
  Queued* slot = reserveSlot();
  return slot && commitSlot(slot, buildSendStatusReq(slot->frame, MAX_FRAME_SIZE, publicKey), tag);
}

bool MeshCompanionClient::logout(const uint8_t* publicKey) {
  Queued* slot = reserveSlot();
  return slot && commitSlot(slot, buildLogout(slot->frame, MAX_FRAME_SIZE, publicKey), 0);
}

bool MeshCompanionClient::discover(uint8_t filter, uint32_t tag) {
  if (radioInfo.firmwareVer < 8) return false;
  Queued* slot = reserveSlot();
  return slot && commitSlot(slot, buildDiscoverRequest(slot->frame, MAX_FRAME_SIZE, filter, tag), tag);
}

bool MeshCompanionClient::transmit(const uint8_t* frame, size_t len, uint32_t tag, bool user, uint32_t nowMs) {
  if (len == 0 || !send(sendCtx, frame, len)) return false;
  inFlight = true;
  inFlightUser = user;
  inFlightCmd = frame[0];
  inFlightTag = tag;
  deadlineMs = nowMs + COMMAND_TIMEOUT_MS;
  return true;
}

void MeshCompanionClient::pump(uint32_t nowMs) {
  if (inFlight || currentStage == Stage::Idle || currentStage == Stage::Failed) return;

  if (currentStage != Stage::Ready) {
    sendStageCommand(nowMs);
    return;
  }

  if (queueCount > 0) {
    const Queued& item = queue[queueHead];
    if (transmit(item.frame, item.len, item.tag, true, nowMs)) {
      if (item.frame[0] == cmd::GET_CONTACT_BY_KEY) memcpy(requestedKey, &item.frame[1], PUB_KEY_SIZE);
      queueHead = (queueHead + 1) % QUEUE_CAPACITY;
      queueCount--;
    }
    return;
  }

  uint8_t frame[8];
  if (syncWanted) {
    if (transmit(frame, buildSyncNextMessage(frame, sizeof(frame)), 0, false, nowMs)) syncWanted = false;
  } else if (contactsDirty) {
    if (transmit(frame, buildGetContacts(frame, sizeof(frame), contactsSince), 0, false, nowMs)) contactsDirty = false;
  }
}

void MeshCompanionClient::sendStageCommand(uint32_t nowMs) {
  uint8_t frame[8 + NAME_FIELD_SIZE];
  size_t len = 0;
  switch (currentStage) {
    case Stage::DeviceQuery:
      len = buildDeviceQuery(frame, sizeof(frame));
      break;
    case Stage::AppStart:
      len = buildAppStart(frame, sizeof(frame), appName);
      break;
    case Stage::GetTime:
      len = buildGetDeviceTime(frame, sizeof(frame));
      break;
    case Stage::SetTime:
      len = buildSetDeviceTime(frame, sizeof(frame), setTimeEpoch + (nowMs - startMs) / 1000);
      break;
    case Stage::Contacts:
      len = buildGetContacts(frame, sizeof(frame), contactsSince);
      break;
    case Stage::Channels:
      len = buildGetChannel(frame, sizeof(frame), channelCursor);
      break;
    case Stage::Sync:
      syncGotMessage = false;
      len = buildSyncNextMessage(frame, sizeof(frame));
      break;
    default:
      return;
  }
  transmit(frame, len, 0, false, nowMs);
}

void MeshCompanionClient::enterStage(Stage next) {
  currentStage = next;
  stageRetries = 0;
}

uint8_t MeshCompanionClient::channelSlots() const {
  return radioInfo.maxChannels ? radioInfo.maxChannels : FALLBACK_CHANNEL_SLOTS;
}

void MeshCompanionClient::handlePush(const uint8_t* frame, size_t len) {
  switch (frame[0]) {
    case push::MSG_WAITING:
      syncWanted = true;
      break;
    case push::ADVERT:
    case push::PATH_UPDATED:
      contactsDirty = true;
      break;
    case push::SEND_CONFIRMED: {
      SendConfirmed confirmed;
      if (parseSendConfirmed(frame, len, confirmed)) {
        events.onDeliveryConfirmed(confirmed.ackCode, confirmed.roundTripMs);
      }
      break;
    }
    case push::NEW_ADVERT: {
      Contact contact;
      if (parseContact(frame, len, contact)) events.onDiscoveredContact(contact);
      break;
    }
    case push::CONTACT_DELETED:
      if (len >= 1 + PUB_KEY_SIZE) events.onContactDeleted(&frame[1]);
      break;
    case push::CONTACTS_FULL:
      events.onContactsFull();
      break;
    case push::LOGIN_SUCCESS:
    case push::LOGIN_FAIL: {
      LoginResult result;
      if (parseLoginResult(frame, len, result)) events.onLoginResult(result);
      break;
    }
    case push::STATUS_RESPONSE: {
      ServerStatus status;
      if (parseServerStatus(frame, len, status)) events.onServerStatus(status);
      break;
    }
    case push::CONTROL_DATA: {
      DiscoverReply reply;
      if (parseDiscoverReply(frame, len, reply)) events.onDiscoverReply(reply);
      break;
    }
    default:
      break;
  }
}

void MeshCompanionClient::handleResponse(const uint8_t* frame, size_t len, uint32_t nowMs) {
  const uint8_t code = frame[0];
  if (code == resp::ERR) {
    finishInFlight(Outcome::RadioError, parseErrCode(frame, len));
    return;
  }

  switch (inFlightCmd) {
    case cmd::DEVICE_QUERY:
      if (!parseDeviceInfo(frame, len, radioInfo)) return;
      events.onDeviceInfo(radioInfo);
      break;
    case cmd::APP_START:
      if (!parseSelfInfo(frame, len, nodeInfo)) return;
      events.onSelfInfo(nodeInfo);
      break;
    case cmd::GET_DEVICE_TIME: {
      uint32_t epochSecs = 0;
      if (!parseCurrTime(frame, len, epochSecs)) return;
      events.onDeviceTime(epochSecs);
      break;
    }
    case cmd::GET_CONTACTS: {
      uint32_t value = 0;
      Contact contact;
      if (parseContactsStart(frame, len, value)) {
        events.onContactsBegin(value);
      } else if (parseContact(frame, len, contact)) {
        events.onContact(contact);
      } else if (parseEndOfContacts(frame, len, value)) {
        if (value > contactsSince) contactsSince = value;
        events.onContactsEnd(contactsSince);
        break;
      } else {
        return;
      }
      // The list is still streaming: each frame restarts the timeout.
      deadlineMs = nowMs + COMMAND_TIMEOUT_MS;
      return;
    }
    case cmd::GET_CHANNEL: {
      Channel channel;
      if (!parseChannelInfo(frame, len, channel)) return;
      if (channel.name[0] != 0) {
        events.onChannel(channel);
      } else if (inFlightUser) {
        events.onChannelEmpty(channel.index);
      }
      break;
    }
    case cmd::SEND_LOGIN:
    case cmd::SEND_STATUS_REQ: {
      SentInfo info;
      if (!parseSent(frame, len, info)) return;
      events.onRequestSent(inFlightCmd, inFlightTag, info);
      break;
    }
    case cmd::SYNC_NEXT_MESSAGE: {
      Message message;
      if (parseMessage(frame, len, message)) {
        events.onMessage(message);
        syncGotMessage = true;
      } else if (code == resp::CHANNEL_DATA_RECV) {
        // Binary datagrams are not shown, but more messages may follow.
        syncGotMessage = true;
      } else if (code != resp::NO_MORE_MESSAGES) {
        return;
      }
      break;
    }
    case cmd::SEND_TXT_MSG: {
      SentInfo info;
      if (!parseSent(frame, len, info)) return;
      events.onDirectAccepted(inFlightTag, info);
      break;
    }
    case cmd::SEND_CHANNEL_TXT_MSG:
      // The firmware answers OK; the published guide says SENT. Accept both.
      if (code != resp::OK && code != resp::SENT) return;
      events.onChannelAccepted(inFlightTag);
      break;
    case cmd::GET_CONTACT_BY_KEY: {
      Contact contact;
      if (code != resp::CONTACT || !parseContact(frame, len, contact)) return;
      // A contact frame left over from a list that timed out, or the late
      // answer to an earlier read, is not the one asked for. Keep waiting.
      if (memcmp(contact.publicKey, requestedKey, PUB_KEY_SIZE) != 0) return;
      events.onContactFetched(inFlightTag, contact);
      break;
    }
    case cmd::GET_BATT_AND_STORAGE: {
      BatteryInfo battery;
      if (!parseBattAndStorage(frame, len, battery)) return;
      events.onBattery(battery);
      break;
    }
    default:
      if (code != resp::OK) return;
      if (inFlightUser) events.onCommandDone(inFlightCmd, inFlightTag);
      break;
  }
  finishInFlight(Outcome::Ok, 0);
}

void MeshCompanionClient::finishInFlight(Outcome outcome, uint8_t errCode) {
  const uint8_t finishedCmd = inFlightCmd;
  inFlight = false;

  if (inFlightUser) {
    if (outcome != Outcome::Ok) {
      events.onCommandFailed(finishedCmd, inFlightTag, toFailReason(outcome == Outcome::Timeout), errCode);
    }
    return;
  }

  if (currentStage != Stage::Ready) {
    advanceHandshake(finishedCmd, outcome);
    return;
  }

  if (finishedCmd == cmd::SYNC_NEXT_MESSAGE && outcome == Outcome::Ok && syncGotMessage) {
    syncGotMessage = false;
    syncWanted = true;
  }
}

void MeshCompanionClient::advanceHandshake(uint8_t finishedCmd, Outcome outcome) {
  const bool ok = outcome == Outcome::Ok;
  switch (currentStage) {
    case Stage::DeviceQuery:
    case Stage::AppStart:
      if (ok) {
        enterStage(currentStage == Stage::DeviceQuery ? Stage::AppStart : Stage::GetTime);
      } else if (stageRetries < HANDSHAKE_RETRIES) {
        stageRetries++;
      } else {
        currentStage = Stage::Failed;
        events.onHandshakeFailed(finishedCmd, toFailReason(outcome == Outcome::Timeout));
      }
      break;
    // The remaining stages are best effort: a failure skips ahead.
    case Stage::GetTime:
      enterStage(setTimeEpoch != 0 ? Stage::SetTime : Stage::Contacts);
      break;
    case Stage::SetTime:
      enterStage(Stage::Contacts);
      break;
    case Stage::Contacts:
      channelCursor = 0;
      enterStage(Stage::Channels);
      break;
    case Stage::Channels:
      if (outcome == Outcome::Timeout) {
        // Messages are matched to channels by slot, so the list must be
        // complete: a slot that stays silent fails the handshake.
        if (stageRetries < HANDSHAKE_RETRIES) {
          stageRetries++;
        } else {
          currentStage = Stage::Failed;
          events.onHandshakeFailed(finishedCmd, FailReason::Timeout);
        }
        break;
      }
      // An error reply means the index is past the radio's last slot.
      if (ok && channelCursor + 1 < channelSlots()) {
        channelCursor++;
        stageRetries = 0;
      } else {
        enterStage(Stage::Sync);
      }
      break;
    case Stage::Sync:
      if (ok && syncGotMessage) break;  // stay: fetch the next queued message
      enterStage(Stage::Ready);
      events.onReady();
      break;
    default:
      break;
  }
}

}  // namespace meshc
