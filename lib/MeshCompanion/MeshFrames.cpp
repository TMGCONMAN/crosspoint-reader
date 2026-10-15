#include "MeshFrames.h"

#include <cstring>

namespace meshc {

namespace {

// Offsets of RESP_CODE_CONTACT fields after the code byte.
constexpr size_t CONTACT_NAME_OFFSET = 1 + PUB_KEY_SIZE + 3 + MAX_PATH_SIZE;
constexpr size_t CONTACT_MIN_SIZE = CONTACT_NAME_OFFSET + NAME_FIELD_SIZE + 4;
constexpr size_t CONTACT_FULL_SIZE = CONTACT_MIN_SIZE + 12;

constexpr size_t SELF_INFO_NAME_OFFSET = 58;
constexpr size_t DEVICE_INFO_V3_SIZE = 80;
constexpr size_t CHANNEL_INFO_SIZE = 2 + NAME_FIELD_SIZE + CHANNEL_SECRET_SIZE;

// Control data types (the high four bits of the first payload byte).
constexpr uint8_t CONTROL_DISCOVER_REQ = 0x80;
constexpr uint8_t CONTROL_DISCOVER_RESP = 0x90;

// Copies a fixed-width, possibly unterminated field and always terminates dst.
void copyField(char* dst, size_t dstSize, const uint8_t* src, size_t fieldLen) {
  size_t n = 0;
  while (n < fieldLen && n + 1 < dstSize && src[n] != 0) {
    dst[n] = static_cast<char>(src[n]);
    n++;
  }
  dst[n] = 0;
}

int32_t readI32(const uint8_t* p) { return static_cast<int32_t>(readU32(p)); }

size_t buildKeyCommand(uint8_t* out, size_t capacity, uint8_t code, const uint8_t* publicKey) {
  if (publicKey == nullptr || capacity < 1 + PUB_KEY_SIZE) return 0;
  out[0] = code;
  memcpy(&out[1], publicKey, PUB_KEY_SIZE);
  return 1 + PUB_KEY_SIZE;
}

}  // namespace

uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

void writeU32(uint8_t* p, uint32_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
  p[2] = static_cast<uint8_t>(value >> 16);
  p[3] = static_cast<uint8_t>(value >> 24);
}

size_t buildDeviceQuery(uint8_t* out, size_t capacity, uint8_t appVersion) {
  if (capacity < 2) return 0;
  out[0] = cmd::DEVICE_QUERY;
  out[1] = appVersion;
  return 2;
}

size_t buildAppStart(uint8_t* out, size_t capacity, const char* appName) {
  const size_t nameLen = appName ? strnlen(appName, NAME_FIELD_SIZE) : 0;
  if (capacity < 8 + nameLen) return 0;
  out[0] = cmd::APP_START;
  out[1] = APP_PROTOCOL_VERSION;
  memset(&out[2], 0, 6);  // reserved
  if (nameLen > 0) memcpy(&out[8], appName, nameLen);
  return 8 + nameLen;
}

size_t buildGetContacts(uint8_t* out, size_t capacity, uint32_t since) {
  if (capacity < 5) return 0;
  out[0] = cmd::GET_CONTACTS;
  if (since == 0) return 1;
  writeU32(&out[1], since);
  return 5;
}

size_t buildGetDeviceTime(uint8_t* out, size_t capacity) {
  if (capacity < 1) return 0;
  out[0] = cmd::GET_DEVICE_TIME;
  return 1;
}

size_t buildSetDeviceTime(uint8_t* out, size_t capacity, uint32_t epochSecs) {
  if (capacity < 5) return 0;
  out[0] = cmd::SET_DEVICE_TIME;
  writeU32(&out[1], epochSecs);
  return 5;
}

size_t buildSyncNextMessage(uint8_t* out, size_t capacity) {
  if (capacity < 1) return 0;
  out[0] = cmd::SYNC_NEXT_MESSAGE;
  return 1;
}

size_t buildSendTxtMsg(uint8_t* out, size_t capacity, uint8_t attempt, uint32_t timestamp, const uint8_t* keyPrefix,
                       const char* text, size_t textLen) {
  constexpr size_t HEADER = 7 + KEY_PREFIX_SIZE;
  if (keyPrefix == nullptr || text == nullptr || textLen == 0 || textLen > MAX_TEXT_BYTES) return 0;
  if (capacity < HEADER + textLen) return 0;
  out[0] = cmd::SEND_TXT_MSG;
  out[1] = txt::PLAIN;
  out[2] = attempt;
  writeU32(&out[3], timestamp);
  memcpy(&out[7], keyPrefix, KEY_PREFIX_SIZE);
  memcpy(&out[HEADER], text, textLen);
  return HEADER + textLen;
}

size_t buildSendChannelTxtMsg(uint8_t* out, size_t capacity, uint8_t channelIndex, uint32_t timestamp, const char* text,
                              size_t textLen) {
  constexpr size_t HEADER = 7;
  if (text == nullptr || textLen == 0 || textLen > MAX_TEXT_BYTES) return 0;
  if (capacity < HEADER + textLen) return 0;
  out[0] = cmd::SEND_CHANNEL_TXT_MSG;
  out[1] = txt::PLAIN;
  out[2] = channelIndex;
  writeU32(&out[3], timestamp);
  memcpy(&out[HEADER], text, textLen);
  return HEADER + textLen;
}

size_t buildGetChannel(uint8_t* out, size_t capacity, uint8_t channelIndex) {
  if (capacity < 2) return 0;
  out[0] = cmd::GET_CHANNEL;
  out[1] = channelIndex;
  return 2;
}

size_t buildSetChannel(uint8_t* out, size_t capacity, const Channel& channel) {
  if (capacity < CHANNEL_INFO_SIZE) return 0;
  out[0] = cmd::SET_CHANNEL;
  out[1] = channel.index;
  memset(&out[2], 0, NAME_FIELD_SIZE);
  // The radio reads the name with strcpy, so the last byte must stay zero.
  memcpy(&out[2], channel.name, strnlen(channel.name, NAME_FIELD_SIZE - 1));
  memcpy(&out[2 + NAME_FIELD_SIZE], channel.secret, CHANNEL_SECRET_SIZE);
  return CHANNEL_INFO_SIZE;
}

size_t buildGetBattAndStorage(uint8_t* out, size_t capacity) {
  if (capacity < 1) return 0;
  out[0] = cmd::GET_BATT_AND_STORAGE;
  return 1;
}

size_t buildSendSelfAdvert(uint8_t* out, size_t capacity, bool flood) {
  if (capacity < 2) return 0;
  out[0] = cmd::SEND_SELF_ADVERT;
  out[1] = flood ? 1 : 0;
  return 2;
}

size_t buildSetAdvertName(uint8_t* out, size_t capacity, const char* name) {
  const size_t nameLen = name ? strnlen(name, NAME_FIELD_SIZE - 1) : 0;
  if (nameLen == 0 || capacity < 1 + nameLen) return 0;
  out[0] = cmd::SET_ADVERT_NAME;
  memcpy(&out[1], name, nameLen);
  return 1 + nameLen;
}

size_t buildSetRadioTxPower(uint8_t* out, size_t capacity, int8_t dbm) {
  if (capacity < 2) return 0;
  out[0] = cmd::SET_RADIO_TX_POWER;
  out[1] = static_cast<uint8_t>(dbm);
  return 2;
}

size_t buildRemoveContact(uint8_t* out, size_t capacity, const uint8_t* publicKey) {
  return buildKeyCommand(out, capacity, cmd::REMOVE_CONTACT, publicKey);
}

size_t buildGetContactByKey(uint8_t* out, size_t capacity, const uint8_t* publicKey) {
  return buildKeyCommand(out, capacity, cmd::GET_CONTACT_BY_KEY, publicKey);
}

size_t buildAddUpdateContact(uint8_t* out, size_t capacity, const Contact& contact) {
  // The radio takes the position only as a pair, and keeps its own when the
  // frame ends before it.
  const size_t size = CONTACT_MIN_SIZE + (contact.hasPosition ? 8 : 0);
  if (capacity < size) return 0;
  size_t i = 0;
  out[i++] = cmd::ADD_UPDATE_CONTACT;
  memcpy(&out[i], contact.publicKey, PUB_KEY_SIZE);
  i += PUB_KEY_SIZE;
  out[i++] = contact.type;
  out[i++] = contact.flags;
  out[i++] = static_cast<uint8_t>(contact.outPathLen);
  memcpy(&out[i], contact.outPath, MAX_PATH_SIZE);
  i += MAX_PATH_SIZE;
  memset(&out[i], 0, NAME_FIELD_SIZE);
  memcpy(&out[i], contact.name, strnlen(contact.name, NAME_FIELD_SIZE - 1));
  i += NAME_FIELD_SIZE;
  writeU32(&out[i], contact.lastAdvert);
  i += 4;
  if (contact.hasPosition) {
    writeU32(&out[i], static_cast<uint32_t>(contact.latE6));
    writeU32(&out[i + 4], static_cast<uint32_t>(contact.lonE6));
    i += 8;
  }
  return i;
}

size_t buildResetPath(uint8_t* out, size_t capacity, const uint8_t* publicKey) {
  return buildKeyCommand(out, capacity, cmd::RESET_PATH, publicKey);
}

size_t buildSetRadioParams(uint8_t* out, size_t capacity, uint32_t freqKhz, uint32_t bwHz, uint8_t spreadingFactor,
                           uint8_t codingRate, int8_t repeat) {
  if (capacity < 12) return 0;
  out[0] = cmd::SET_RADIO_PARAMS;
  writeU32(&out[1], freqKhz);
  writeU32(&out[5], bwHz);
  out[9] = spreadingFactor;
  out[10] = codingRate;
  if (repeat < 0) return 11;
  out[11] = repeat ? 1 : 0;
  return 12;
}

size_t buildSetAdvertLatLon(uint8_t* out, size_t capacity, int32_t latE6, int32_t lonE6) {
  if (capacity < 9) return 0;
  if (latE6 < -90000000 || latE6 > 90000000 || lonE6 < -180000000 || lonE6 > 180000000) return 0;
  out[0] = cmd::SET_ADVERT_LATLON;
  writeU32(&out[1], static_cast<uint32_t>(latE6));
  writeU32(&out[5], static_cast<uint32_t>(lonE6));
  return 9;
}

size_t buildSetOtherParams(uint8_t* out, size_t capacity, const SelfInfo& self) {
  if (capacity < 5) return 0;
  out[0] = cmd::SET_OTHER_PARAMS;
  out[1] = self.manualAddContacts;
  out[2] = self.telemetryModes;
  out[3] = self.advertLocPolicy;
  out[4] = self.multiAcks;
  return 5;
}

size_t buildSendLogin(uint8_t* out, size_t capacity, const uint8_t* publicKey, const char* password) {
  // The radio terminates the password at the end of the frame.
  const size_t passwordLen = password ? strnlen(password, 15) : 0;
  if (publicKey == nullptr || capacity < 1 + PUB_KEY_SIZE + passwordLen) return 0;
  out[0] = cmd::SEND_LOGIN;
  memcpy(&out[1], publicKey, PUB_KEY_SIZE);
  if (passwordLen > 0) memcpy(&out[1 + PUB_KEY_SIZE], password, passwordLen);
  return 1 + PUB_KEY_SIZE + passwordLen;
}

size_t buildSendStatusReq(uint8_t* out, size_t capacity, const uint8_t* publicKey) {
  return buildKeyCommand(out, capacity, cmd::SEND_STATUS_REQ, publicKey);
}

size_t buildLogout(uint8_t* out, size_t capacity, const uint8_t* publicKey) {
  return buildKeyCommand(out, capacity, cmd::LOGOUT, publicKey);
}

size_t buildDiscoverRequest(uint8_t* out, size_t capacity, uint8_t filter, uint32_t tag) {
  if (capacity < 11) return 0;
  out[0] = cmd::SEND_CONTROL_DATA;
  out[1] = CONTROL_DISCOVER_REQ;  // full keys in the answers
  out[2] = filter;
  writeU32(&out[3], tag);
  writeU32(&out[7], 0);  // since: every node, however long ago it changed
  return 11;
}

bool parseDeviceInfo(const uint8_t* frame, size_t len, DeviceInfo& out) {
  if (len < 2 || frame[0] != resp::DEVICE_INFO) return false;
  out = DeviceInfo{};
  out.firmwareVer = frame[1];
  if (out.firmwareVer < 3 || len < DEVICE_INFO_V3_SIZE) return true;
  out.maxContacts = static_cast<uint16_t>(frame[2]) * 2;
  out.maxChannels = frame[3];
  out.blePin = readU32(&frame[4]);
  copyField(out.buildDate, sizeof(out.buildDate), &frame[8], 12);
  copyField(out.model, sizeof(out.model), &frame[20], 40);
  copyField(out.version, sizeof(out.version), &frame[60], 20);
  if (out.firmwareVer >= 9 && len > DEVICE_INFO_V3_SIZE) {
    out.hasClientRepeat = true;
    out.clientRepeat = frame[DEVICE_INFO_V3_SIZE] != 0;
  }
  return true;
}

bool parseSelfInfo(const uint8_t* frame, size_t len, SelfInfo& out) {
  if (len < SELF_INFO_NAME_OFFSET || frame[0] != resp::SELF_INFO) return false;
  out = SelfInfo{};
  out.advType = frame[1];
  out.txPowerDbm = static_cast<int8_t>(frame[2]);
  out.maxTxPowerDbm = static_cast<int8_t>(frame[3]);
  memcpy(out.publicKey, &frame[4], PUB_KEY_SIZE);
  out.latE6 = readI32(&frame[36]);
  out.lonE6 = readI32(&frame[40]);
  out.multiAcks = frame[44];
  out.advertLocPolicy = frame[45];
  out.telemetryModes = frame[46];
  out.manualAddContacts = frame[47];
  out.freqKhz = readU32(&frame[48]);
  out.bwHz = readU32(&frame[52]);
  out.spreadingFactor = frame[56];
  out.codingRate = frame[57];
  copyField(out.name, sizeof(out.name), &frame[SELF_INFO_NAME_OFFSET], len - SELF_INFO_NAME_OFFSET);
  return true;
}

bool parseContact(const uint8_t* frame, size_t len, Contact& out) {
  if (len < CONTACT_MIN_SIZE) return false;
  if (frame[0] != resp::CONTACT && frame[0] != push::NEW_ADVERT) return false;
  out = Contact{};
  size_t i = 1;
  memcpy(out.publicKey, &frame[i], PUB_KEY_SIZE);
  i += PUB_KEY_SIZE;
  out.type = frame[i++];
  out.flags = frame[i++];
  out.outPathLen = static_cast<int8_t>(frame[i++]);
  memcpy(out.outPath, &frame[i], MAX_PATH_SIZE);
  i += MAX_PATH_SIZE;
  copyField(out.name, sizeof(out.name), &frame[i], NAME_FIELD_SIZE);
  i += NAME_FIELD_SIZE;
  out.lastAdvert = readU32(&frame[i]);
  i += 4;
  if (len >= CONTACT_FULL_SIZE) {
    out.latE6 = readI32(&frame[i]);
    out.lonE6 = readI32(&frame[i + 4]);
    out.lastMod = readU32(&frame[i + 8]);
    out.hasPosition = true;
  }
  return true;
}

bool parseContactsStart(const uint8_t* frame, size_t len, uint32_t& count) {
  if (len < 5 || frame[0] != resp::CONTACTS_START) return false;
  count = readU32(&frame[1]);
  return true;
}

bool parseEndOfContacts(const uint8_t* frame, size_t len, uint32_t& mostRecentLastMod) {
  if (len < 1 || frame[0] != resp::END_OF_CONTACTS) return false;
  mostRecentLastMod = len >= 5 ? readU32(&frame[1]) : 0;
  return true;
}

bool parseChannelInfo(const uint8_t* frame, size_t len, Channel& out) {
  if (len < CHANNEL_INFO_SIZE || frame[0] != resp::CHANNEL_INFO) return false;
  out = Channel{};
  out.index = frame[1];
  copyField(out.name, sizeof(out.name), &frame[2], NAME_FIELD_SIZE);
  memcpy(out.secret, &frame[2 + NAME_FIELD_SIZE], CHANNEL_SECRET_SIZE);
  return true;
}

bool parseMessage(const uint8_t* frame, size_t len, Message& out) {
  if (len < 1) return false;
  const uint8_t code = frame[0];
  const bool v3 = code == resp::CONTACT_MSG_RECV_V3 || code == resp::CHANNEL_MSG_RECV_V3;
  const bool channel = code == resp::CHANNEL_MSG_RECV || code == resp::CHANNEL_MSG_RECV_V3;
  if (!v3 && code != resp::CONTACT_MSG_RECV && code != resp::CHANNEL_MSG_RECV) return false;

  // Fixed part: [snr + 2 reserved] + (channel index | 6-byte key prefix) + path_len + txt_type + timestamp.
  const size_t headerLen = 1 + (v3 ? 3 : 0) + (channel ? 1 : KEY_PREFIX_SIZE) + 2 + 4;
  if (len < headerLen) return false;

  out = Message{};
  out.isChannel = channel;
  size_t i = 1;
  if (v3) {
    out.hasSnr = true;
    out.snrQuarterDb = static_cast<int8_t>(frame[i]);
    i += 3;
  }
  if (channel) {
    out.channelIndex = frame[i++];
  } else {
    memcpy(out.senderPrefix, &frame[i], KEY_PREFIX_SIZE);
    i += KEY_PREFIX_SIZE;
  }
  out.pathLen = frame[i++];
  out.textType = frame[i++];
  out.timestamp = readU32(&frame[i]);
  i += 4;
  if (out.textType == txt::SIGNED_PLAIN) {
    // Signed messages carry a 4-byte signer prefix before the text.
    if (len < i + 4) return false;
    memcpy(out.signerPrefix, &frame[i], sizeof(out.signerPrefix));
    i += 4;
  }
  size_t textLen = len - i;
  if (textLen > sizeof(out.text) - 1) textLen = sizeof(out.text) - 1;
  memcpy(out.text, &frame[i], textLen);
  out.text[textLen] = 0;
  out.textLen = static_cast<uint16_t>(textLen);
  return true;
}

bool parseSent(const uint8_t* frame, size_t len, SentInfo& out) {
  if (len < 10 || frame[0] != resp::SENT) return false;
  out.flood = frame[1] == 1;
  out.expectedAck = readU32(&frame[2]);
  out.timeoutMs = readU32(&frame[6]);
  return true;
}

bool parseSendConfirmed(const uint8_t* frame, size_t len, SendConfirmed& out) {
  if (len < 9 || frame[0] != push::SEND_CONFIRMED) return false;
  out.ackCode = readU32(&frame[1]);
  out.roundTripMs = readU32(&frame[5]);
  return true;
}

bool parseBattAndStorage(const uint8_t* frame, size_t len, BatteryInfo& out) {
  if (len < 3 || frame[0] != resp::BATT_AND_STORAGE) return false;
  out = BatteryInfo{};
  out.milliVolts = static_cast<uint16_t>(frame[1] | (frame[2] << 8));
  if (len >= 11) {
    out.hasStorage = true;
    out.usedKb = readU32(&frame[3]);
    out.totalKb = readU32(&frame[7]);
  }
  return true;
}

bool parseCurrTime(const uint8_t* frame, size_t len, uint32_t& epochSecs) {
  if (len < 5 || frame[0] != resp::CURR_TIME) return false;
  epochSecs = readU32(&frame[1]);
  return true;
}

bool parseLoginResult(const uint8_t* frame, size_t len, LoginResult& out) {
  if (len < 2 + KEY_PREFIX_SIZE) return false;
  if (frame[0] != push::LOGIN_SUCCESS && frame[0] != push::LOGIN_FAIL) return false;
  out = LoginResult{};
  out.success = frame[0] == push::LOGIN_SUCCESS;
  // Bit 0 of the permissions byte is admin; legacy servers send 0.
  out.isAdmin = out.success && (frame[1] & 0x01) != 0;
  memcpy(out.keyPrefix, &frame[2], KEY_PREFIX_SIZE);
  return true;
}

bool parseServerStatus(const uint8_t* frame, size_t len, ServerStatus& out) {
  // Code, reserved, key prefix, then the server's stats structure.
  constexpr size_t STATS = 2 + KEY_PREFIX_SIZE;
  if (len < STATS + 44 || frame[0] != push::STATUS_RESPONSE) return false;
  const auto u16 = [frame](size_t at) { return static_cast<uint16_t>(frame[at] | (frame[at + 1] << 8)); };
  out = ServerStatus{};
  memcpy(out.keyPrefix, &frame[2], KEY_PREFIX_SIZE);
  out.battMilliVolts = u16(STATS);
  out.txQueueLen = u16(STATS + 2);
  out.noiseFloor = static_cast<int16_t>(u16(STATS + 4));
  out.lastRssi = static_cast<int16_t>(u16(STATS + 6));
  out.packetsReceived = readU32(&frame[STATS + 8]);
  out.packetsSent = readU32(&frame[STATS + 12]);
  out.airTimeSecs = readU32(&frame[STATS + 16]);
  out.upTimeSecs = readU32(&frame[STATS + 20]);
  out.sentFlood = readU32(&frame[STATS + 24]);
  out.sentDirect = readU32(&frame[STATS + 28]);
  out.receivedFlood = readU32(&frame[STATS + 32]);
  out.receivedDirect = readU32(&frame[STATS + 36]);
  out.errorEvents = u16(STATS + 40);
  out.lastSnrQuarterDb = static_cast<int16_t>(u16(STATS + 42));
  return true;
}

bool parseDiscoverReply(const uint8_t* frame, size_t len, DiscoverReply& out) {
  // Code, SNR, RSSI, path length, then the control payload:
  // type | node type, SNR there, tag, public key.
  constexpr size_t PAYLOAD = 4;
  if (len < PAYLOAD + 6 + PUB_KEY_SIZE || frame[0] != push::CONTROL_DATA) return false;
  if ((frame[PAYLOAD] & 0xF0) != CONTROL_DISCOVER_RESP) return false;
  out = DiscoverReply{};
  out.snrHereQuarterDb = static_cast<int8_t>(frame[1]);
  out.rssiHere = static_cast<int8_t>(frame[2]);
  out.nodeType = frame[PAYLOAD] & 0x0F;
  out.snrThereQuarterDb = static_cast<int8_t>(frame[PAYLOAD + 1]);
  out.tag = readU32(&frame[PAYLOAD + 2]);
  memcpy(out.publicKey, &frame[PAYLOAD + 6], PUB_KEY_SIZE);
  return true;
}

uint8_t parseErrCode(const uint8_t* frame, size_t len) {
  if (len < 2 || frame[0] != resp::ERR) return 0;
  return frame[1];
}

bool splitChannelText(const char* text, size_t textLen, size_t& nameLen, size_t& bodyOffset) {
  if (text == nullptr) return false;
  for (size_t i = 1; i + 1 < textLen; i++) {
    if (text[i] == ':' && text[i + 1] == ' ') {
      nameLen = i;
      bodyOffset = i + 2;
      return true;
    }
  }
  return false;
}

size_t maxChannelTextBytes(const char* nodeName) {
  const size_t overhead = (nodeName ? strnlen(nodeName, NAME_FIELD_SIZE) : 0) + 2;
  return MAX_TEXT_BYTES - overhead;
}

}  // namespace meshc
