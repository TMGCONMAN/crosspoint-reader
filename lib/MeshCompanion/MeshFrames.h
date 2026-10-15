#pragma once

#include <cstddef>
#include <cstdint>

// Frame codec for the MeshCore companion radio protocol. One frame is one BLE
// write or notification; the first byte is the command, response or push code.
// All multi-byte integers are little-endian. Layouts follow
// examples/companion_radio/MyMesh.cpp in the MeshCore repository.
namespace meshc {

constexpr size_t MAX_FRAME_SIZE = 176;
constexpr size_t PUB_KEY_SIZE = 32;
constexpr size_t KEY_PREFIX_SIZE = 6;
constexpr size_t MAX_PATH_SIZE = 64;
constexpr size_t NAME_FIELD_SIZE = 32;
constexpr size_t CHANNEL_SECRET_SIZE = 16;
constexpr size_t MAX_TEXT_BYTES = 160;

// Asking for version 3 makes the radio send message frames that carry SNR.
constexpr uint8_t APP_PROTOCOL_VERSION = 3;
// path_len value for a message that arrived by direct route, not flood.
constexpr uint8_t PATH_LEN_DIRECT = 0xFF;
// Codes from this value up are pushes; lower codes answer a command.
constexpr uint8_t FIRST_PUSH_CODE = 0x80;

namespace cmd {
constexpr uint8_t APP_START = 1;
constexpr uint8_t SEND_TXT_MSG = 2;
constexpr uint8_t SEND_CHANNEL_TXT_MSG = 3;
constexpr uint8_t GET_CONTACTS = 4;
constexpr uint8_t GET_DEVICE_TIME = 5;
constexpr uint8_t SET_DEVICE_TIME = 6;
constexpr uint8_t SEND_SELF_ADVERT = 7;
constexpr uint8_t SET_ADVERT_NAME = 8;
constexpr uint8_t ADD_UPDATE_CONTACT = 9;
constexpr uint8_t SYNC_NEXT_MESSAGE = 10;
constexpr uint8_t SET_RADIO_PARAMS = 11;
constexpr uint8_t SET_RADIO_TX_POWER = 12;
constexpr uint8_t RESET_PATH = 13;
constexpr uint8_t SET_ADVERT_LATLON = 14;
constexpr uint8_t REMOVE_CONTACT = 15;
constexpr uint8_t GET_BATT_AND_STORAGE = 20;
constexpr uint8_t DEVICE_QUERY = 22;
constexpr uint8_t GET_CONTACT_BY_KEY = 30;
constexpr uint8_t SEND_LOGIN = 26;
constexpr uint8_t SEND_STATUS_REQ = 27;
constexpr uint8_t LOGOUT = 29;
constexpr uint8_t GET_CHANNEL = 31;
constexpr uint8_t SET_CHANNEL = 32;
constexpr uint8_t SET_OTHER_PARAMS = 38;
constexpr uint8_t SEND_CONTROL_DATA = 55;  // firmware version 8 and later
}  // namespace cmd

namespace resp {
constexpr uint8_t OK = 0;
constexpr uint8_t ERR = 1;
constexpr uint8_t CONTACTS_START = 2;
constexpr uint8_t CONTACT = 3;
constexpr uint8_t END_OF_CONTACTS = 4;
constexpr uint8_t SELF_INFO = 5;
constexpr uint8_t SENT = 6;
constexpr uint8_t CONTACT_MSG_RECV = 7;
constexpr uint8_t CHANNEL_MSG_RECV = 8;
constexpr uint8_t CURR_TIME = 9;
constexpr uint8_t NO_MORE_MESSAGES = 10;
constexpr uint8_t BATT_AND_STORAGE = 12;
constexpr uint8_t DEVICE_INFO = 13;
constexpr uint8_t CONTACT_MSG_RECV_V3 = 16;
constexpr uint8_t CHANNEL_MSG_RECV_V3 = 17;
constexpr uint8_t CHANNEL_INFO = 18;
constexpr uint8_t CHANNEL_DATA_RECV = 27;
}  // namespace resp

namespace push {
constexpr uint8_t ADVERT = 0x80;
constexpr uint8_t PATH_UPDATED = 0x81;
constexpr uint8_t SEND_CONFIRMED = 0x82;
constexpr uint8_t MSG_WAITING = 0x83;
constexpr uint8_t LOGIN_SUCCESS = 0x85;
constexpr uint8_t LOGIN_FAIL = 0x86;
constexpr uint8_t STATUS_RESPONSE = 0x87;
constexpr uint8_t NEW_ADVERT = 0x8A;
constexpr uint8_t CONTROL_DATA = 0x8E;
constexpr uint8_t CONTACT_DELETED = 0x8F;
constexpr uint8_t CONTACTS_FULL = 0x90;
}  // namespace push

namespace err {
constexpr uint8_t UNSUPPORTED_CMD = 1;
constexpr uint8_t NOT_FOUND = 2;
constexpr uint8_t TABLE_FULL = 3;
constexpr uint8_t BAD_STATE = 4;
constexpr uint8_t FILE_IO_ERROR = 5;
constexpr uint8_t ILLEGAL_ARG = 6;
}  // namespace err

namespace txt {
constexpr uint8_t PLAIN = 0;
constexpr uint8_t CLI_DATA = 1;
constexpr uint8_t SIGNED_PLAIN = 2;
}  // namespace txt

namespace adv {
constexpr uint8_t NONE = 0;
constexpr uint8_t CHAT = 1;
constexpr uint8_t REPEATER = 2;
constexpr uint8_t ROOM = 3;
}  // namespace adv

struct DeviceInfo {
  uint8_t firmwareVer = 0;
  // The fields below are zero when the radio reports firmware version < 3.
  uint16_t maxContacts = 0;
  uint8_t maxChannels = 0;
  uint32_t blePin = 0;
  char buildDate[13] = {0};
  char model[41] = {0};
  char version[21] = {0};
  // Firmware version 9 and later: the radio also repeats as a client.
  bool hasClientRepeat = false;
  bool clientRepeat = false;
};

struct SelfInfo {
  uint8_t advType = 0;
  int8_t txPowerDbm = 0;
  int8_t maxTxPowerDbm = 0;
  uint8_t publicKey[PUB_KEY_SIZE] = {0};
  int32_t latE6 = 0;
  int32_t lonE6 = 0;
  uint8_t multiAcks = 0;
  uint8_t advertLocPolicy = 0;
  uint8_t telemetryModes = 0;
  uint8_t manualAddContacts = 0;
  uint32_t freqKhz = 0;  // 915000 = 915.000 MHz
  uint32_t bwHz = 0;     // 250000 = 250 kHz
  uint8_t spreadingFactor = 0;
  uint8_t codingRate = 0;
  char name[NAME_FIELD_SIZE + 1] = {0};
};

// Contact::flags bit the MeshCore apps use for a favourite. The radio also
// never overwrites a favourite when its contact table is full.
constexpr uint8_t CONTACT_FLAG_FAVORITE = 0x01;

struct Contact {
  uint8_t publicKey[PUB_KEY_SIZE] = {0};
  uint8_t type = 0;  // one of adv::*
  uint8_t flags = 0;
  int8_t outPathLen = 0;  // the radio's byte as is: -1 (0xFF) when it knows no route
  uint8_t outPath[MAX_PATH_SIZE] = {0};
  char name[NAME_FIELD_SIZE + 1] = {0};
  uint32_t lastAdvert = 0;
  int32_t latE6 = 0;
  int32_t lonE6 = 0;
  uint32_t lastMod = 0;
  // False when the frame ended before the position and lastMod fields.
  bool hasPosition = false;
};

struct Channel {
  uint8_t index = 0;
  char name[NAME_FIELD_SIZE + 1] = {0};
  uint8_t secret[CHANNEL_SECRET_SIZE] = {0};
};

struct Message {
  bool isChannel = false;
  uint8_t channelIndex = 0;                     // channel messages only
  uint8_t senderPrefix[KEY_PREFIX_SIZE] = {0};  // contact messages only
  bool hasSnr = false;
  int8_t snrQuarterDb = 0;  // SNR in dB times 4
  uint8_t pathLen = 0;      // hop count for flood, PATH_LEN_DIRECT for direct
  uint8_t textType = 0;     // one of txt::*
  uint32_t timestamp = 0;   // sender's clock, epoch seconds
  // txt::SIGNED_PLAIN only: the first bytes of the author's key. A room
  // server posts other people's messages this way.
  uint8_t signerPrefix[4] = {0};
  uint16_t textLen = 0;
  char text[MAX_FRAME_SIZE + 1] = {0};  // UTF-8, always null terminated
};

struct SentInfo {
  bool flood = false;
  uint32_t expectedAck = 0;  // matches a later SEND_CONFIRMED push; 0 = no ack expected
  uint32_t timeoutMs = 0;    // radio's estimate of the round trip
};

struct SendConfirmed {
  uint32_t ackCode = 0;
  uint32_t roundTripMs = 0;
};

struct BatteryInfo {
  uint16_t milliVolts = 0;
  bool hasStorage = false;
  uint32_t usedKb = 0;
  uint32_t totalKb = 0;
};

// The answer of a repeater or room server to a login.
struct LoginResult {
  bool success = false;
  bool isAdmin = false;
  uint8_t keyPrefix[KEY_PREFIX_SIZE] = {0};
};

// A repeater's or room server's counters, the part both report alike.
struct ServerStatus {
  uint8_t keyPrefix[KEY_PREFIX_SIZE] = {0};
  uint16_t battMilliVolts = 0;
  uint16_t txQueueLen = 0;
  int16_t noiseFloor = 0;
  int16_t lastRssi = 0;
  uint32_t packetsReceived = 0;
  uint32_t packetsSent = 0;
  uint32_t airTimeSecs = 0;
  uint32_t upTimeSecs = 0;
  uint32_t sentFlood = 0;
  uint32_t sentDirect = 0;
  uint32_t receivedFlood = 0;
  uint32_t receivedDirect = 0;
  uint16_t errorEvents = 0;
  int16_t lastSnrQuarterDb = 0;
};

// A node in range answering a discovery request (repeaters only, in the
// MeshCore firmware of 2026).
struct DiscoverReply {
  int8_t snrHereQuarterDb = 0;  // how this radio heard the answer
  int8_t rssiHere = 0;
  int8_t snrThereQuarterDb = 0;  // how that node heard the request
  uint8_t nodeType = 0;          // one of adv::*
  uint32_t tag = 0;
  uint8_t publicKey[PUB_KEY_SIZE] = {0};
};

// Discovery request bit for each node type that should answer.
constexpr uint8_t DISCOVER_FILTER_REPEATER = 1 << adv::REPEATER;
constexpr uint8_t DISCOVER_FILTER_ROOM = 1 << adv::ROOM;

uint32_t readU32(const uint8_t* p);
void writeU32(uint8_t* p, uint32_t value);

// Builders write one command frame into out and return its length, or 0 when
// the arguments are invalid or capacity is too small.
size_t buildDeviceQuery(uint8_t* out, size_t capacity, uint8_t appVersion = APP_PROTOCOL_VERSION);
size_t buildAppStart(uint8_t* out, size_t capacity, const char* appName);
size_t buildGetContacts(uint8_t* out, size_t capacity, uint32_t since);
size_t buildGetDeviceTime(uint8_t* out, size_t capacity);
size_t buildSetDeviceTime(uint8_t* out, size_t capacity, uint32_t epochSecs);
size_t buildSyncNextMessage(uint8_t* out, size_t capacity);
size_t buildSendTxtMsg(uint8_t* out, size_t capacity, uint8_t attempt, uint32_t timestamp, const uint8_t* keyPrefix,
                       const char* text, size_t textLen);
size_t buildSendChannelTxtMsg(uint8_t* out, size_t capacity, uint8_t channelIndex, uint32_t timestamp, const char* text,
                              size_t textLen);
size_t buildGetChannel(uint8_t* out, size_t capacity, uint8_t channelIndex);
size_t buildSetChannel(uint8_t* out, size_t capacity, const Channel& channel);
size_t buildGetBattAndStorage(uint8_t* out, size_t capacity);
size_t buildSendSelfAdvert(uint8_t* out, size_t capacity, bool flood);
size_t buildSetAdvertName(uint8_t* out, size_t capacity, const char* name);
size_t buildSetRadioTxPower(uint8_t* out, size_t capacity, int8_t dbm);
size_t buildRemoveContact(uint8_t* out, size_t capacity, const uint8_t* publicKey);
size_t buildGetContactByKey(uint8_t* out, size_t capacity, const uint8_t* publicKey);
// Writes back every field the radio stores for a contact it already has, so
// pass a contact read with buildGetContactByKey() and change only what must
// change. lastMod is left out; the radio then stamps the current time.
size_t buildAddUpdateContact(uint8_t* out, size_t capacity, const Contact& contact);
size_t buildResetPath(uint8_t* out, size_t capacity, const uint8_t* publicKey);
// freqKhz and bwHz in the units SelfInfo reports them. Firmware 9 and later
// reads a client-repeat byte after them and turns repeating off when it is
// missing, so pass what DeviceInfo reported; repeat < 0 leaves the byte out.
size_t buildSetRadioParams(uint8_t* out, size_t capacity, uint32_t freqKhz, uint32_t bwHz, uint8_t spreadingFactor,
                           uint8_t codingRate, int8_t repeat = -1);
size_t buildSetAdvertLatLon(uint8_t* out, size_t capacity, int32_t latE6, int32_t lonE6);
// Writes the four values SelfInfo reports as they are, so change one field of
// a SelfInfo and pass the rest unchanged.
size_t buildSetOtherParams(uint8_t* out, size_t capacity, const SelfInfo& self);
// An empty password is a guest login.
size_t buildSendLogin(uint8_t* out, size_t capacity, const uint8_t* publicKey, const char* password);
size_t buildSendStatusReq(uint8_t* out, size_t capacity, const uint8_t* publicKey);
size_t buildLogout(uint8_t* out, size_t capacity, const uint8_t* publicKey);
// A zero-hop request that nodes in range answer with push::CONTROL_DATA.
size_t buildDiscoverRequest(uint8_t* out, size_t capacity, uint8_t filter, uint32_t tag);

// Parsers return false when the frame is too short or has the wrong code.
bool parseDeviceInfo(const uint8_t* frame, size_t len, DeviceInfo& out);
bool parseSelfInfo(const uint8_t* frame, size_t len, SelfInfo& out);
// Accepts resp::CONTACT and push::NEW_ADVERT, which share one layout.
bool parseContact(const uint8_t* frame, size_t len, Contact& out);
bool parseContactsStart(const uint8_t* frame, size_t len, uint32_t& count);
bool parseEndOfContacts(const uint8_t* frame, size_t len, uint32_t& mostRecentLastMod);
bool parseChannelInfo(const uint8_t* frame, size_t len, Channel& out);
// Accepts the four contact and channel message codes (7, 8, 16, 17).
bool parseMessage(const uint8_t* frame, size_t len, Message& out);
bool parseSent(const uint8_t* frame, size_t len, SentInfo& out);
bool parseSendConfirmed(const uint8_t* frame, size_t len, SendConfirmed& out);
bool parseBattAndStorage(const uint8_t* frame, size_t len, BatteryInfo& out);
bool parseCurrTime(const uint8_t* frame, size_t len, uint32_t& epochSecs);
// Accepts push::LOGIN_SUCCESS and push::LOGIN_FAIL.
bool parseLoginResult(const uint8_t* frame, size_t len, LoginResult& out);
bool parseServerStatus(const uint8_t* frame, size_t len, ServerStatus& out);
// False for control data that is not a discovery answer.
bool parseDiscoverReply(const uint8_t* frame, size_t len, DiscoverReply& out);
// Error code from a resp::ERR frame; 0 when the radio sent none.
uint8_t parseErrCode(const uint8_t* frame, size_t len);

// Channel messages arrive as "<sender>: <text>". On success nameLen is the
// sender's length and bodyOffset the index of the first text byte. The sender
// name is not authenticated.
bool splitChannelText(const char* text, size_t textLen, size_t& nameLen, size_t& bodyOffset);

// Largest channel text the radio can send for a node with this name.
size_t maxChannelTextBytes(const char* nodeName);

}  // namespace meshc
