#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "MeshFrames.h"
#include "RadioFrames.h"

using namespace meshc;

namespace {

std::vector<uint8_t> built(const uint8_t* buf, size_t len) { return std::vector<uint8_t>(buf, buf + len); }

}  // namespace

TEST(MeshFramesBuild, ChannelMessageMatchesProtocolGuideExample) {
  // "Hello" to channel 1 at 1234567890, from docs.meshcore.io/companion_protocol.
  uint8_t buf[MAX_FRAME_SIZE];
  const size_t len = buildSendChannelTxtMsg(buf, sizeof(buf), 1, 1234567890, "Hello", 5);
  const std::vector<uint8_t> expected = {0x03, 0x00, 0x01, 0xD2, 0x02, 0x96, 0x49, 0x48, 0x65, 0x6C, 0x6C, 0x6F};
  EXPECT_EQ(built(buf, len), expected);
}

TEST(MeshFramesBuild, DeviceQueryAsksForProtocolVersion3) {
  uint8_t buf[4];
  ASSERT_EQ(buildDeviceQuery(buf, sizeof(buf)), 2u);
  EXPECT_EQ(buf[0], 22);
  EXPECT_EQ(buf[1], 3);
}

TEST(MeshFramesBuild, AppStartPutsNameAfterSevenReservedBytes) {
  uint8_t buf[64];
  const size_t len = buildAppStart(buf, sizeof(buf), "CrossPoint");
  ASSERT_EQ(len, 8u + 10u);
  EXPECT_EQ(buf[0], 1);
  for (int i = 2; i < 8; i++) EXPECT_EQ(buf[i], 0) << i;
  EXPECT_EQ(std::string(reinterpret_cast<char*>(&buf[8]), 10), "CrossPoint");
}

TEST(MeshFramesBuild, AppStartWithoutNameStillMeetsRadioMinimumLength) {
  uint8_t buf[16];
  EXPECT_EQ(buildAppStart(buf, sizeof(buf), nullptr), 8u);  // firmware requires len >= 8
}

TEST(MeshFramesBuild, GetContactsOmitsSinceWhenZero) {
  uint8_t buf[8];
  EXPECT_EQ(buildGetContacts(buf, sizeof(buf), 0), 1u);
  ASSERT_EQ(buildGetContacts(buf, sizeof(buf), 0x01020304), 5u);
  EXPECT_EQ(built(buf, 5), (std::vector<uint8_t>{4, 0x04, 0x03, 0x02, 0x01}));
}

TEST(MeshFramesBuild, DirectMessageLayout) {
  const uint8_t prefix[KEY_PREFIX_SIZE] = {1, 2, 3, 4, 5, 6};
  uint8_t buf[MAX_FRAME_SIZE];
  const size_t len = buildSendTxtMsg(buf, sizeof(buf), 2, 0x11223344, prefix, "hi", 2);
  const std::vector<uint8_t> expected = {2, 0, 2, 0x44, 0x33, 0x22, 0x11, 1, 2, 3, 4, 5, 6, 'h', 'i'};
  EXPECT_EQ(built(buf, len), expected);
}

TEST(MeshFramesBuild, DirectMessageRejectsEmptyAndOversizeText) {
  const uint8_t prefix[KEY_PREFIX_SIZE] = {0};
  uint8_t buf[MAX_FRAME_SIZE];
  const std::string longest(MAX_TEXT_BYTES, 'x');
  const std::string tooLong(MAX_TEXT_BYTES + 1, 'x');
  EXPECT_EQ(buildSendTxtMsg(buf, sizeof(buf), 0, 1, prefix, "", 0), 0u);
  EXPECT_EQ(buildSendTxtMsg(buf, sizeof(buf), 0, 1, prefix, tooLong.data(), tooLong.size()), 0u);
  // The longest message must leave one spare byte: the radio writes a
  // terminator at frame[len] in a MAX_FRAME_SIZE buffer.
  const size_t len = buildSendTxtMsg(buf, sizeof(buf), 0, 1, prefix, longest.data(), longest.size());
  EXPECT_EQ(len, 13u + MAX_TEXT_BYTES);
  EXPECT_LT(len, MAX_FRAME_SIZE);
}

TEST(MeshFramesBuild, BuildersRejectShortBuffers) {
  const uint8_t prefix[KEY_PREFIX_SIZE] = {0};
  const uint8_t key[PUB_KEY_SIZE] = {0};
  uint8_t buf[4];
  EXPECT_EQ(buildSendTxtMsg(buf, sizeof(buf), 0, 1, prefix, "hello", 5), 0u);
  EXPECT_EQ(buildSendChannelTxtMsg(buf, sizeof(buf), 0, 1, "hello", 5), 0u);
  EXPECT_EQ(buildAppStart(buf, sizeof(buf), "x"), 0u);
  EXPECT_EQ(buildRemoveContact(buf, sizeof(buf), key), 0u);
  EXPECT_EQ(buildSetChannel(buf, sizeof(buf), Channel{}), 0u);
}

TEST(MeshFramesBuild, SetChannelIsFiftyBytesWithPaddedName) {
  Channel channel;
  channel.index = 3;
  strcpy(channel.name, "hikers");
  memset(channel.secret, 0x7E, sizeof(channel.secret));
  uint8_t buf[MAX_FRAME_SIZE];
  ASSERT_EQ(buildSetChannel(buf, sizeof(buf), channel), 50u);
  EXPECT_EQ(buf[0], 32);
  EXPECT_EQ(buf[1], 3);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(&buf[2])), "hikers");
  EXPECT_EQ(buf[2 + 6], 0);
  EXPECT_EQ(buf[33], 0);
  EXPECT_EQ(buf[34], 0x7E);
  EXPECT_EQ(buf[49], 0x7E);
}

TEST(MeshFramesBuild, KeyCommandsCarryThePublicKey) {
  uint8_t key[PUB_KEY_SIZE];
  for (size_t i = 0; i < PUB_KEY_SIZE; i++) key[i] = static_cast<uint8_t>(i);
  uint8_t buf[MAX_FRAME_SIZE];
  ASSERT_EQ(buildRemoveContact(buf, sizeof(buf), key), 33u);
  EXPECT_EQ(buf[0], 15);
  EXPECT_EQ(memcmp(&buf[1], key, PUB_KEY_SIZE), 0);
  ASSERT_EQ(buildResetPath(buf, sizeof(buf), key), 33u);
  EXPECT_EQ(buf[0], 13);
}

TEST(MeshFramesParse, DeviceInfoFromCurrentFirmware) {
  const auto frame = radio::deviceInfo(13, 175, 40, 654321);
  ASSERT_EQ(frame.size(), 82u);
  DeviceInfo info;
  ASSERT_TRUE(parseDeviceInfo(frame.data(), frame.size(), info));
  EXPECT_EQ(info.firmwareVer, 13);
  EXPECT_EQ(info.maxContacts, 350);
  EXPECT_EQ(info.maxChannels, 40);
  EXPECT_EQ(info.blePin, 654321u);
  EXPECT_STREQ(info.buildDate, "30 Sep 2026");
  EXPECT_STREQ(info.model, "Heltec V3");
  EXPECT_STREQ(info.version, "v1.13.0");
}

TEST(MeshFramesParse, DeviceInfoFromOldFirmwareHasOnlyVersion) {
  const uint8_t frame[] = {resp::DEVICE_INFO, 2};
  DeviceInfo info;
  ASSERT_TRUE(parseDeviceInfo(frame, sizeof(frame), info));
  EXPECT_EQ(info.firmwareVer, 2);
  EXPECT_EQ(info.maxChannels, 0);
}

TEST(MeshFramesParse, DeviceInfoFieldWithoutTerminatorIsBounded) {
  auto frame = radio::deviceInfo(13, 50, 8);
  for (size_t i = 20; i < 60; i++) frame[i] = 'M';  // model fills all 40 bytes
  DeviceInfo info;
  ASSERT_TRUE(parseDeviceInfo(frame.data(), frame.size(), info));
  EXPECT_EQ(strlen(info.model), 40u);
}

TEST(MeshFramesParse, SelfInfo) {
  const auto frame = radio::selfInfo("Trailhead");
  SelfInfo info;
  ASSERT_TRUE(parseSelfInfo(frame.data(), frame.size(), info));
  EXPECT_EQ(info.advType, adv::CHAT);
  EXPECT_EQ(info.txPowerDbm, 20);
  EXPECT_EQ(info.maxTxPowerDbm, 22);
  EXPECT_EQ(info.publicKey[0], 0xA1);
  EXPECT_EQ(info.publicKey[31], 0xA1);
  EXPECT_EQ(info.latE6, 40712800);
  EXPECT_EQ(info.lonE6, -74006000);
  EXPECT_EQ(info.freqKhz, 910525u);
  EXPECT_EQ(info.bwHz, 62500u);
  EXPECT_EQ(info.spreadingFactor, 7);
  EXPECT_EQ(info.codingRate, 5);
  EXPECT_STREQ(info.name, "Trailhead");
}

TEST(MeshFramesParse, SelfInfoWithEmptyNameAndTruncatedFrame) {
  auto frame = radio::selfInfo("");
  SelfInfo info;
  ASSERT_TRUE(parseSelfInfo(frame.data(), frame.size(), info));
  EXPECT_STREQ(info.name, "");
  frame.pop_back();
  EXPECT_FALSE(parseSelfInfo(frame.data(), frame.size(), info));
}

TEST(MeshFramesParse, Contact) {
  const auto frame = radio::contact("Basecamp", 0xB2, 1790001234, adv::REPEATER, -1);
  ASSERT_EQ(frame.size(), 148u);
  Contact contact;
  ASSERT_TRUE(parseContact(frame.data(), frame.size(), contact));
  EXPECT_EQ(contact.publicKey[0], 0xB2);
  EXPECT_EQ(contact.type, adv::REPEATER);
  EXPECT_EQ(contact.outPathLen, -1);
  EXPECT_EQ(contact.outPath[63], 0x33);
  EXPECT_STREQ(contact.name, "Basecamp");
  EXPECT_EQ(contact.lastAdvert, 1790000000u);
  EXPECT_EQ(contact.latE6, 1000000);
  EXPECT_EQ(contact.lonE6, 2000000);
  EXPECT_EQ(contact.lastMod, 1790001234u);
}

TEST(MeshFramesParse, ContactRejectsShortFrameAndWrongCode) {
  auto frame = radio::contact("x", 1, 1);
  Contact contact;
  EXPECT_FALSE(parseContact(frame.data(), 100, contact));
  frame[0] = resp::SELF_INFO;
  EXPECT_FALSE(parseContact(frame.data(), frame.size(), contact));
}

TEST(MeshFramesParse, ContactNameFillingTheFieldIsTerminated) {
  const auto frame = radio::contact(std::string(NAME_FIELD_SIZE, 'N'), 1, 1);
  Contact contact;
  ASSERT_TRUE(parseContact(frame.data(), frame.size(), contact));
  EXPECT_EQ(strlen(contact.name), NAME_FIELD_SIZE);
}

TEST(MeshFramesParse, ChannelInfo) {
  const auto frame = radio::channelInfo(4, "Public", 0x8B);
  Channel channel;
  ASSERT_TRUE(parseChannelInfo(frame.data(), frame.size(), channel));
  EXPECT_EQ(channel.index, 4);
  EXPECT_STREQ(channel.name, "Public");
  EXPECT_EQ(channel.secret[0], 0x8B);
  EXPECT_EQ(channel.secret[15], 0x8B);
  EXPECT_FALSE(parseChannelInfo(frame.data(), frame.size() - 1, channel));
}

TEST(MeshFramesParse, ContactMessageV3) {
  const auto frame = radio::contactMessageV3(0xC3, "see you at 5", 1790000100, -30, 3);
  Message message;
  ASSERT_TRUE(parseMessage(frame.data(), frame.size(), message));
  EXPECT_FALSE(message.isChannel);
  EXPECT_TRUE(message.hasSnr);
  EXPECT_EQ(message.snrQuarterDb, -30);
  EXPECT_EQ(message.senderPrefix[0], 0xC3);
  EXPECT_EQ(message.senderPrefix[5], 0xC3);
  EXPECT_EQ(message.pathLen, 3);
  EXPECT_EQ(message.textType, txt::PLAIN);
  EXPECT_EQ(message.timestamp, 1790000100u);
  EXPECT_STREQ(message.text, "see you at 5");
  EXPECT_EQ(message.textLen, 12);
}

TEST(MeshFramesParse, ChannelMessageV3) {
  const auto frame = radio::channelMessageV3(2, "Ana: on the ridge", 1790000200);
  Message message;
  ASSERT_TRUE(parseMessage(frame.data(), frame.size(), message));
  EXPECT_TRUE(message.isChannel);
  EXPECT_EQ(message.channelIndex, 2);
  EXPECT_EQ(message.snrQuarterDb, -12);
  EXPECT_EQ(message.pathLen, 2);
  EXPECT_EQ(message.timestamp, 1790000200u);
  EXPECT_STREQ(message.text, "Ana: on the ridge");
}

TEST(MeshFramesParse, LegacyMessageFramesHaveNoSnr) {
  std::vector<uint8_t> contactFrame = {resp::CONTACT_MSG_RECV, 9, 9, 9, 9, 9, 9, PATH_LEN_DIRECT, txt::PLAIN};
  radio::putU32(contactFrame, 77);
  contactFrame.push_back('y');
  Message message;
  ASSERT_TRUE(parseMessage(contactFrame.data(), contactFrame.size(), message));
  EXPECT_FALSE(message.hasSnr);
  EXPECT_EQ(message.senderPrefix[0], 9);
  EXPECT_EQ(message.pathLen, PATH_LEN_DIRECT);
  EXPECT_EQ(message.timestamp, 77u);
  EXPECT_STREQ(message.text, "y");

  std::vector<uint8_t> channelFrame = {resp::CHANNEL_MSG_RECV, 5, 1, txt::PLAIN};
  radio::putU32(channelFrame, 88);
  channelFrame.push_back('z');
  ASSERT_TRUE(parseMessage(channelFrame.data(), channelFrame.size(), message));
  EXPECT_TRUE(message.isChannel);
  EXPECT_EQ(message.channelIndex, 5);
  EXPECT_EQ(message.timestamp, 88u);
  EXPECT_STREQ(message.text, "z");
}

TEST(MeshFramesParse, SignedMessageSkipsSignerPrefix) {
  const auto frame = radio::contactMessageV3(1, "signed text", 5, 0, PATH_LEN_DIRECT, txt::SIGNED_PLAIN);
  Message message;
  ASSERT_TRUE(parseMessage(frame.data(), frame.size(), message));
  EXPECT_EQ(message.textType, txt::SIGNED_PLAIN);
  EXPECT_STREQ(message.text, "signed text");
}

TEST(MeshFramesParse, MessageWithEmptyTextAndTruncatedHeader) {
  auto frame = radio::contactMessageV3(1, "", 5);
  Message message;
  ASSERT_TRUE(parseMessage(frame.data(), frame.size(), message));
  EXPECT_EQ(message.textLen, 0);
  EXPECT_FALSE(parseMessage(frame.data(), frame.size() - 1, message));
  const auto signedFrame = radio::contactMessageV3(1, "", 5, 0, PATH_LEN_DIRECT, txt::SIGNED_PLAIN);
  EXPECT_FALSE(parseMessage(signedFrame.data(), signedFrame.size() - 1, message));
}

TEST(MeshFramesParse, MessageFillingTheWholeFrame) {
  auto frame = radio::channelMessageV3(0, "", 1);
  frame.resize(MAX_FRAME_SIZE, 'q');
  Message message;
  ASSERT_TRUE(parseMessage(frame.data(), frame.size(), message));
  EXPECT_EQ(message.textLen, MAX_FRAME_SIZE - 11);
  EXPECT_EQ(strlen(message.text), MAX_FRAME_SIZE - 11);
}

TEST(MeshFramesParse, MessageRejectsOtherCodes) {
  const auto frame = radio::noMoreMessages();
  Message message;
  EXPECT_FALSE(parseMessage(frame.data(), frame.size(), message));
}

TEST(MeshFramesParse, SentAndSendConfirmed) {
  const auto sentFrame = radio::sent(true, 0xDEADBEEF, 12000);
  SentInfo info;
  ASSERT_TRUE(parseSent(sentFrame.data(), sentFrame.size(), info));
  EXPECT_TRUE(info.flood);
  EXPECT_EQ(info.expectedAck, 0xDEADBEEFu);
  EXPECT_EQ(info.timeoutMs, 12000u);

  const auto confirmedFrame = radio::sendConfirmed(0xDEADBEEF, 2345);
  SendConfirmed confirmed;
  ASSERT_TRUE(parseSendConfirmed(confirmedFrame.data(), confirmedFrame.size(), confirmed));
  EXPECT_EQ(confirmed.ackCode, 0xDEADBEEFu);
  EXPECT_EQ(confirmed.roundTripMs, 2345u);
}

TEST(MeshFramesParse, BatteryWithAndWithoutStorage) {
  const auto full = radio::battery(4012, 120, 4096);
  BatteryInfo info;
  ASSERT_TRUE(parseBattAndStorage(full.data(), full.size(), info));
  EXPECT_EQ(info.milliVolts, 4012);
  EXPECT_TRUE(info.hasStorage);
  EXPECT_EQ(info.usedKb, 120u);
  EXPECT_EQ(info.totalKb, 4096u);
  ASSERT_TRUE(parseBattAndStorage(full.data(), 3, info));
  EXPECT_EQ(info.milliVolts, 4012);
  EXPECT_FALSE(info.hasStorage);
}

TEST(MeshFramesParse, ErrorCodeIsOptional) {
  const uint8_t withCode[] = {resp::ERR, err::TABLE_FULL};
  const uint8_t bare[] = {resp::ERR};
  EXPECT_EQ(parseErrCode(withCode, sizeof(withCode)), err::TABLE_FULL);
  EXPECT_EQ(parseErrCode(bare, sizeof(bare)), 0);
}

TEST(MeshFramesText, SplitChannelText) {
  size_t nameLen = 0;
  size_t bodyOffset = 0;
  const std::string text = "Ana: meet at 5: ok";
  ASSERT_TRUE(splitChannelText(text.data(), text.size(), nameLen, bodyOffset));
  EXPECT_EQ(text.substr(0, nameLen), "Ana");
  EXPECT_EQ(text.substr(bodyOffset), "meet at 5: ok");

  const std::string noSender = "no separator here";
  EXPECT_FALSE(splitChannelText(noSender.data(), noSender.size(), nameLen, bodyOffset));
  const std::string leading = ": odd";
  EXPECT_FALSE(splitChannelText(leading.data(), leading.size(), nameLen, bodyOffset));
}

TEST(MeshFramesText, ChannelTextLimitDependsOnNodeName) {
  EXPECT_EQ(maxChannelTextBytes("Ana"), 155u);
  EXPECT_EQ(maxChannelTextBytes(""), 158u);
}

// Field order of updateContactFromFrame() in the firmware's MyMesh.cpp.
TEST(MeshFramesBuild, AddUpdateContactWritesBackWhatWasRead) {
  // 0x4A: favourite bit clear, other flag bits set. Path length 0x42: two
  // hops with 2-byte entries.
  const radio::Frame read = radio::contact("Ana", 0xC3, 77, adv::CHAT, 0x42, resp::CONTACT, 0x4A);
  Contact contact;
  ASSERT_TRUE(parseContact(read.data(), read.size(), contact));
  ASSERT_TRUE(contact.hasPosition);
  contact.flags |= CONTACT_FLAG_FAVORITE;

  uint8_t out[MAX_FRAME_SIZE];
  const size_t len = buildAddUpdateContact(out, sizeof(out), contact);
  // The read frame without its 4-byte lastMod: the radio stamps its own.
  ASSERT_EQ(len, read.size() - 4);
  EXPECT_EQ(out[0], cmd::ADD_UPDATE_CONTACT);
  EXPECT_EQ(out[1 + PUB_KEY_SIZE + 1], 0x4B);  // flags: only the favourite bit changed
  radio::Frame expected(read.begin(), read.end() - 4);
  expected[0] = cmd::ADD_UPDATE_CONTACT;
  expected[1 + PUB_KEY_SIZE + 1] = 0x4B;
  EXPECT_EQ(radio::Frame(out, out + len), expected);
}

TEST(MeshFramesBuild, AddUpdateContactLeavesOutAPositionItNeverRead) {
  radio::Frame shortRead = radio::contact("Ana", 0xC3, 77);
  shortRead.resize(shortRead.size() - 12);  // no position, no lastMod
  Contact contact;
  ASSERT_TRUE(parseContact(shortRead.data(), shortRead.size(), contact));
  EXPECT_FALSE(contact.hasPosition);

  uint8_t out[MAX_FRAME_SIZE];
  EXPECT_EQ(buildAddUpdateContact(out, sizeof(out), contact), shortRead.size());
  EXPECT_EQ(buildAddUpdateContact(out, shortRead.size() - 1, contact), 0u);
}

TEST(MeshFramesBuild, AddUpdateContactKeepsHighPathLengthsAndLongNames) {
  // 0x83: three hops with 3-byte entries, a value above 127. The radio never
  // sends a name longer than 31 bytes.
  const std::string longName(NAME_FIELD_SIZE - 1, 'n');
  const radio::Frame read = radio::contact(longName, 0xC3, 77, adv::CHAT, static_cast<int8_t>(0x83));
  Contact contact;
  ASSERT_TRUE(parseContact(read.data(), read.size(), contact));

  uint8_t out[MAX_FRAME_SIZE];
  const size_t len = buildAddUpdateContact(out, sizeof(out), contact);
  radio::Frame expected(read.begin(), read.end() - 4);
  expected[0] = cmd::ADD_UPDATE_CONTACT;
  EXPECT_EQ(radio::Frame(out, out + len), expected);
  EXPECT_EQ(out[1 + PUB_KEY_SIZE + 2], 0x83);
  EXPECT_EQ(out[1 + PUB_KEY_SIZE + 3 + MAX_PATH_SIZE + NAME_FIELD_SIZE - 1], 0);  // name stays terminated
}

TEST(MeshFramesBuild, GetContactByKey) {
  uint8_t key[PUB_KEY_SIZE];
  memset(key, 0xC3, sizeof(key));
  uint8_t out[MAX_FRAME_SIZE];
  ASSERT_EQ(buildGetContactByKey(out, sizeof(out), key), 1 + PUB_KEY_SIZE);
  EXPECT_EQ(out[0], cmd::GET_CONTACT_BY_KEY);
  EXPECT_EQ(memcmp(&out[1], key, sizeof(key)), 0);
  EXPECT_EQ(buildGetContactByKey(out, sizeof(out), nullptr), 0u);
}

TEST(MeshFramesParse, SignedMessageKeepsSignerPrefix) {
  // V3 contact message from a room server: author prefix AA BB CC DD.
  radio::Frame frame = {16, 8, 0, 0, 1,    2,    3,    4,    5,   6,  0xFF, txt::SIGNED_PLAIN,
                        1,  0, 0, 0, 0xAA, 0xBB, 0xCC, 0xDD, 'h', 'i'};
  Message message;
  ASSERT_TRUE(parseMessage(frame.data(), frame.size(), message));
  EXPECT_EQ(std::string(message.text), "hi");
  EXPECT_EQ(message.signerPrefix[0], 0xAA);
  EXPECT_EQ(message.signerPrefix[3], 0xDD);
}

TEST(MeshFramesBuild, RadioParamsInSelfInfoUnits) {
  uint8_t out[MAX_FRAME_SIZE];
  ASSERT_EQ(buildSetRadioParams(out, sizeof(out), 869525, 250000, 11, 5), 11u);
  EXPECT_EQ(out[0], cmd::SET_RADIO_PARAMS);
  EXPECT_EQ(readU32(&out[1]), 869525u);
  EXPECT_EQ(readU32(&out[5]), 250000u);
  EXPECT_EQ(out[9], 11);
  EXPECT_EQ(out[10], 5);
}

TEST(MeshFramesBuild, AdvertPositionRejectsOutOfRange) {
  uint8_t out[MAX_FRAME_SIZE];
  ASSERT_EQ(buildSetAdvertLatLon(out, sizeof(out), -33868800, 151209300), 9u);
  EXPECT_EQ(static_cast<int32_t>(readU32(&out[1])), -33868800);
  EXPECT_EQ(static_cast<int32_t>(readU32(&out[5])), 151209300);
  EXPECT_EQ(buildSetAdvertLatLon(out, sizeof(out), 91000000, 0), 0u);
  EXPECT_EQ(buildSetAdvertLatLon(out, sizeof(out), 0, -181000000), 0u);
}

TEST(MeshFramesBuild, OtherParamsWritesBackSelfInfo) {
  SelfInfo self;
  self.manualAddContacts = 1;
  self.telemetryModes = 0x15;
  self.advertLocPolicy = 1;
  self.multiAcks = 2;
  uint8_t out[MAX_FRAME_SIZE];
  ASSERT_EQ(buildSetOtherParams(out, sizeof(out), self), 5u);
  EXPECT_EQ(built(out, 5), (std::vector<uint8_t>{cmd::SET_OTHER_PARAMS, 1, 0x15, 1, 2}));
}

TEST(MeshFramesBuild, LoginCarriesKeyAndPassword) {
  uint8_t key[PUB_KEY_SIZE];
  memset(key, 0x42, sizeof(key));
  uint8_t out[MAX_FRAME_SIZE];
  ASSERT_EQ(buildSendLogin(out, sizeof(out), key, "secret"), 1 + PUB_KEY_SIZE + 6);
  EXPECT_EQ(out[0], cmd::SEND_LOGIN);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(&out[1 + PUB_KEY_SIZE]), 6), "secret");
  // A guest login has no password at all.
  EXPECT_EQ(buildSendLogin(out, sizeof(out), key, ""), 1 + PUB_KEY_SIZE);
  EXPECT_EQ(buildSendStatusReq(out, sizeof(out), key), 1 + PUB_KEY_SIZE);
  EXPECT_EQ(out[0], cmd::SEND_STATUS_REQ);
}

TEST(MeshFramesBuild, DiscoverRequestMatchesRepeaterFirmware) {
  uint8_t out[MAX_FRAME_SIZE];
  ASSERT_EQ(buildDiscoverRequest(out, sizeof(out), DISCOVER_FILTER_REPEATER, 0x01020304), 11u);
  EXPECT_EQ(built(out, 11), (std::vector<uint8_t>{cmd::SEND_CONTROL_DATA, 0x80, 0x04, 4, 3, 2, 1, 0, 0, 0, 0}));
}

TEST(MeshFramesParse, LoginResults) {
  radio::Frame ok = {push::LOGIN_SUCCESS, 1, 1, 2, 3, 4, 5, 6, 0, 0, 0, 0, 3, 1};
  LoginResult result;
  ASSERT_TRUE(parseLoginResult(ok.data(), ok.size(), result));
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.isAdmin);
  EXPECT_EQ(result.keyPrefix[5], 6);
  radio::Frame fail = {push::LOGIN_FAIL, 0, 9, 9, 9, 9, 9, 9};
  ASSERT_TRUE(parseLoginResult(fail.data(), fail.size(), result));
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.isAdmin);
  EXPECT_FALSE(parseLoginResult(fail.data(), fail.size() - 1, result));
}

TEST(MeshFramesParse, ServerStatus) {
  radio::Frame frame = {push::STATUS_RESPONSE, 0, 1, 2, 3, 4, 5, 6};
  const auto u16 = [&frame](uint16_t v) {
    frame.push_back(static_cast<uint8_t>(v));
    frame.push_back(static_cast<uint8_t>(v >> 8));
  };
  const auto u32 = [&frame](uint32_t v) {
    for (int i = 0; i < 4; i++) frame.push_back(static_cast<uint8_t>(v >> (8 * i)));
  };
  u16(4012);                         // battery
  u16(2);                            // queue
  u16(static_cast<uint16_t>(-112));  // noise floor
  u16(static_cast<uint16_t>(-90));   // last RSSI
  u32(1000);                         // received
  u32(500);                          // sent
  u32(60);                           // air time
  u32(86400);                        // up time
  u32(10);
  u32(20);
  u32(30);
  u32(40);
  u16(7);                           // errors
  u16(static_cast<uint16_t>(-20));  // SNR x4
  ServerStatus status;
  ASSERT_TRUE(parseServerStatus(frame.data(), frame.size(), status));
  EXPECT_EQ(status.battMilliVolts, 4012);
  EXPECT_EQ(status.noiseFloor, -112);
  EXPECT_EQ(status.lastRssi, -90);
  EXPECT_EQ(status.upTimeSecs, 86400u);
  EXPECT_EQ(status.receivedDirect, 40u);
  EXPECT_EQ(status.lastSnrQuarterDb, -20);
  EXPECT_FALSE(parseServerStatus(frame.data(), frame.size() - 1, status));
}

TEST(MeshFramesParse, DiscoverReply) {
  radio::Frame frame = {
      push::CONTROL_DATA, static_cast<uint8_t>(-8), static_cast<uint8_t>(-95), 0, 0x90 | adv::REPEATER, 24, 4, 3, 2, 1};
  for (size_t i = 0; i < PUB_KEY_SIZE; i++) frame.push_back(static_cast<uint8_t>(i));
  DiscoverReply reply;
  ASSERT_TRUE(parseDiscoverReply(frame.data(), frame.size(), reply));
  EXPECT_EQ(reply.snrHereQuarterDb, -8);
  EXPECT_EQ(reply.rssiHere, -95);
  EXPECT_EQ(reply.snrThereQuarterDb, 24);
  EXPECT_EQ(reply.nodeType, adv::REPEATER);
  EXPECT_EQ(reply.tag, 0x01020304u);
  EXPECT_EQ(reply.publicKey[31], 31);
  frame[4] = 0x80;  // a request, not an answer
  EXPECT_FALSE(parseDiscoverReply(frame.data(), frame.size(), reply));
}

TEST(MeshFramesBuild, RadioParamsKeepClientRepeat) {
  uint8_t out[MAX_FRAME_SIZE];
  ASSERT_EQ(buildSetRadioParams(out, sizeof(out), 869525, 250000, 11, 5, 1), 12u);
  EXPECT_EQ(out[11], 1);
  ASSERT_EQ(buildSetRadioParams(out, sizeof(out), 869525, 250000, 11, 5, 0), 12u);
  EXPECT_EQ(out[11], 0);
}

TEST(MeshFramesParse, DeviceInfoClientRepeatFromVersion9) {
  radio::Frame frame(82, 0);
  frame[0] = resp::DEVICE_INFO;
  frame[1] = 9;
  frame[80] = 1;
  DeviceInfo info;
  ASSERT_TRUE(parseDeviceInfo(frame.data(), frame.size(), info));
  EXPECT_TRUE(info.hasClientRepeat);
  EXPECT_TRUE(info.clientRepeat);
  frame[1] = 8;
  ASSERT_TRUE(parseDeviceInfo(frame.data(), frame.size(), info));
  EXPECT_FALSE(info.hasClientRepeat);
}
