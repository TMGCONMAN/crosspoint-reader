#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "MeshFrames.h"

// Builds frames the way the companion radio firmware does
// (examples/companion_radio/MyMesh.cpp in the MeshCore repository), so the
// tests check the parsers against the real layouts and not against themselves.
namespace radio {

using Frame = std::vector<uint8_t>;

inline void putU32(Frame& f, uint32_t v) {
  for (int shift = 0; shift < 32; shift += 8) f.push_back(static_cast<uint8_t>(v >> shift));
}

inline void putField(Frame& f, const std::string& text, size_t width) {
  for (size_t i = 0; i < width; i++) f.push_back(i < text.size() ? static_cast<uint8_t>(text[i]) : 0);
}

inline void putBytes(Frame& f, uint8_t value, size_t count) { f.insert(f.end(), count, value); }

inline Frame deviceInfo(uint8_t firmwareVer, uint8_t maxContactsDiv2, uint8_t maxChannels, uint32_t pin = 123456) {
  Frame f = {meshc::resp::DEVICE_INFO, firmwareVer, maxContactsDiv2, maxChannels};
  putU32(f, pin);
  putField(f, "30 Sep 2026", 12);
  putField(f, "Heltec V3", 40);
  putField(f, "v1.13.0", 20);
  f.push_back(0);  // client repeat, v9+
  f.push_back(0);  // path hash mode, v10+
  return f;
}

inline Frame selfInfo(const std::string& name, uint8_t keyFill = 0xA1) {
  Frame f = {meshc::resp::SELF_INFO, meshc::adv::CHAT, 20, 22};
  putBytes(f, keyFill, meshc::PUB_KEY_SIZE);
  putU32(f, static_cast<uint32_t>(40712800));   // lat * 1e6
  putU32(f, static_cast<uint32_t>(-74006000));  // lon * 1e6
  f.push_back(1);                               // multi acks
  f.push_back(0);                               // advert location policy
  f.push_back(0x15);                            // telemetry modes
  f.push_back(0);                               // manual add contacts
  putU32(f, 910525);                            // 910.525 MHz
  putU32(f, 62500);                             // 62.5 kHz
  f.push_back(7);
  f.push_back(5);
  f.insert(f.end(), name.begin(), name.end());  // no terminator
  return f;
}

inline Frame contact(const std::string& name, uint8_t keyFill, uint32_t lastMod, uint8_t type = meshc::adv::CHAT,
                     int8_t outPathLen = -1, uint8_t code = meshc::resp::CONTACT, uint8_t flags = 0) {
  Frame f = {code};
  putBytes(f, keyFill, meshc::PUB_KEY_SIZE);
  f.push_back(type);
  f.push_back(flags);
  f.push_back(static_cast<uint8_t>(outPathLen));
  putBytes(f, 0x33, meshc::MAX_PATH_SIZE);
  putField(f, name, meshc::NAME_FIELD_SIZE);
  putU32(f, 1790000000);  // last advert
  putU32(f, 1000000);     // lat
  putU32(f, 2000000);     // lon
  putU32(f, lastMod);
  return f;
}

inline Frame contactsStart(uint32_t count) {
  Frame f = {meshc::resp::CONTACTS_START};
  putU32(f, count);
  return f;
}

inline Frame endOfContacts(uint32_t lastMod) {
  Frame f = {meshc::resp::END_OF_CONTACTS};
  putU32(f, lastMod);
  return f;
}

inline Frame channelInfo(uint8_t index, const std::string& name, uint8_t secretFill = 0x5C) {
  Frame f = {meshc::resp::CHANNEL_INFO, index};
  putField(f, name, meshc::NAME_FIELD_SIZE);
  putBytes(f, secretFill, meshc::CHANNEL_SECRET_SIZE);
  return f;
}

inline Frame contactMessageV3(uint8_t keyFill, const std::string& text, uint32_t timestamp, int8_t snrQuarterDb = 20,
                              uint8_t pathLen = meshc::PATH_LEN_DIRECT, uint8_t textType = meshc::txt::PLAIN) {
  Frame f = {meshc::resp::CONTACT_MSG_RECV_V3, static_cast<uint8_t>(snrQuarterDb), 0, 0};
  putBytes(f, keyFill, meshc::KEY_PREFIX_SIZE);
  f.push_back(pathLen);
  f.push_back(textType);
  putU32(f, timestamp);
  if (textType == meshc::txt::SIGNED_PLAIN) putBytes(f, 0xEE, 4);
  f.insert(f.end(), text.begin(), text.end());
  return f;
}

inline Frame channelMessageV3(uint8_t channelIndex, const std::string& text, uint32_t timestamp,
                              int8_t snrQuarterDb = -12, uint8_t pathLen = 2) {
  Frame f = {meshc::resp::CHANNEL_MSG_RECV_V3,
             static_cast<uint8_t>(snrQuarterDb),
             0,
             0,
             channelIndex,
             pathLen,
             meshc::txt::PLAIN};
  putU32(f, timestamp);
  f.insert(f.end(), text.begin(), text.end());
  return f;
}

inline Frame sent(bool flood, uint32_t expectedAck, uint32_t timeoutMs) {
  Frame f = {meshc::resp::SENT, static_cast<uint8_t>(flood ? 1 : 0)};
  putU32(f, expectedAck);
  putU32(f, timeoutMs);
  return f;
}

inline Frame sendConfirmed(uint32_t ackCode, uint32_t roundTripMs) {
  Frame f = {meshc::push::SEND_CONFIRMED};
  putU32(f, ackCode);
  putU32(f, roundTripMs);
  return f;
}

inline Frame currTime(uint32_t epochSecs) {
  Frame f = {meshc::resp::CURR_TIME};
  putU32(f, epochSecs);
  return f;
}

inline Frame battery(uint16_t milliVolts, uint32_t usedKb, uint32_t totalKb) {
  Frame f = {meshc::resp::BATT_AND_STORAGE, static_cast<uint8_t>(milliVolts), static_cast<uint8_t>(milliVolts >> 8)};
  putU32(f, usedKb);
  putU32(f, totalKb);
  return f;
}

inline Frame ok() { return {meshc::resp::OK}; }
inline Frame error(uint8_t code) { return {meshc::resp::ERR, code}; }
inline Frame noMoreMessages() { return {meshc::resp::NO_MORE_MESSAGES}; }
inline Frame pushCode(uint8_t code) { return {code}; }

}  // namespace radio
