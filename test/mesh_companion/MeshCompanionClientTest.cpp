#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "MeshCompanionClient.h"
#include "RadioFrames.h"

using namespace meshc;
using Stage = MeshCompanionClient::Stage;

namespace {

struct Failure {
  uint8_t cmd;
  uint32_t tag;
  FailReason reason;
  uint8_t errCode;
};

// Plays the radio: records every frame the client sends and every event it
// raises, and lets a test feed reply frames back in.
class Harness : public MeshEvents {
 public:
  std::vector<radio::Frame> sent;
  bool linkBusy = false;
  uint32_t now = 1000;

  std::vector<std::string> contacts;
  std::vector<std::string> channels;
  std::vector<std::string> messages;
  std::vector<Failure> failures;
  std::vector<uint32_t> directAccepted;
  std::vector<uint32_t> channelAccepted;
  std::vector<uint32_t> confirmedAcks;
  std::vector<uint8_t> done;
  std::vector<std::string> discovered;
  SentInfo lastSent;
  BatteryInfo lastBattery;
  uint32_t deviceTime = 0;
  uint32_t contactsCount = 0;
  uint32_t contactsEnd = 0;
  int readyCount = 0;
  int handshakeFailed = 0;
  int contactsFull = 0;
  int contactDeleted = 0;
  uint32_t fetchedTag = 0;
  Contact fetched;

  MeshCompanionClient client{&Harness::sendThunk, this, *this};

  static bool sendThunk(void* ctx, const uint8_t* frame, size_t len) {
    auto* self = static_cast<Harness*>(ctx);
    if (self->linkBusy) return false;
    self->sent.emplace_back(frame, frame + len);
    return true;
  }

  void feed(const radio::Frame& frame) { client.onFrame(frame.data(), frame.size(), now); }
  void advance(uint32_t ms) {
    now += ms;
    client.poll(now);
  }
  uint8_t lastCmd() const { return sent.empty() ? 0xFF : sent.back()[0]; }
  std::vector<uint8_t> sentCodes() const {
    std::vector<uint8_t> codes;
    for (const auto& frame : sent) codes.push_back(frame[0]);
    return codes;
  }

  // Runs a complete handshake against a radio with no contacts, no channel
  // names and no queued messages.
  void handshake(uint8_t maxChannels = 1, const std::string& nodeName = "Me") {
    client.start({}, now);
    feed(radio::deviceInfo(13, 50, maxChannels));
    feed(radio::selfInfo(nodeName));
    feed(radio::currTime(1790000000));
    feed(radio::contactsStart(0));
    feed(radio::endOfContacts(0));
    for (uint8_t i = 0; i < maxChannels; i++) feed(radio::channelInfo(i, ""));
    feed(radio::noMoreMessages());
    ASSERT_TRUE(client.isReady());
    sent.clear();
  }

  void onDeviceTime(uint32_t epochSecs) override { deviceTime = epochSecs; }
  void onReady() override { readyCount++; }
  void onHandshakeFailed(uint8_t, FailReason) override { handshakeFailed++; }
  void onContactsBegin(uint32_t count) override { contactsCount = count; }
  void onContact(const Contact& contact) override { contacts.emplace_back(contact.name); }
  void onContactsEnd(uint32_t lastMod) override { contactsEnd = lastMod; }
  void onDiscoveredContact(const Contact& contact) override { discovered.emplace_back(contact.name); }
  void onContactDeleted(const uint8_t*) override { contactDeleted++; }
  void onContactFetched(uint32_t tag, const Contact& contact) override {
    fetchedTag = tag;
    fetched = contact;
  }
  void onContactsFull() override { contactsFull++; }
  void onChannel(const Channel& channel) override { channels.emplace_back(channel.name); }
  void onMessage(const Message& message) override { messages.emplace_back(message.text); }
  void onDirectAccepted(uint32_t tag, const SentInfo& info) override {
    directAccepted.push_back(tag);
    lastSent = info;
  }
  void onChannelAccepted(uint32_t tag) override { channelAccepted.push_back(tag); }
  void onDeliveryConfirmed(uint32_t ackCode, uint32_t) override { confirmedAcks.push_back(ackCode); }
  void onBattery(const BatteryInfo& info) override { lastBattery = info; }
  void onCommandDone(uint8_t cmd, uint32_t) override { done.push_back(cmd); }
  void onCommandFailed(uint8_t cmd, uint32_t tag, FailReason reason, uint8_t errCode) override {
    failures.push_back({cmd, tag, reason, errCode});
  }
};

const uint8_t PEER[KEY_PREFIX_SIZE] = {0xC3, 0xC3, 0xC3, 0xC3, 0xC3, 0xC3};

}  // namespace

TEST(MeshClientHandshake, WalksEveryStageInOrder) {
  Harness h;
  h.client.start({}, h.now);
  EXPECT_EQ(h.client.stage(), Stage::DeviceQuery);

  h.feed(radio::deviceInfo(13, 175, 2));
  EXPECT_EQ(h.client.stage(), Stage::AppStart);
  h.feed(radio::selfInfo("Trailhead"));
  EXPECT_EQ(h.client.stage(), Stage::GetTime);
  h.feed(radio::currTime(1790000000));
  EXPECT_EQ(h.client.stage(), Stage::Contacts);

  h.feed(radio::contactsStart(2));
  h.feed(radio::contact("Ana", 0xC3, 100));
  h.feed(radio::contact("Relay 7", 0xD4, 250, adv::REPEATER));
  EXPECT_EQ(h.client.stage(), Stage::Contacts);
  h.feed(radio::endOfContacts(250));
  EXPECT_EQ(h.client.stage(), Stage::Channels);

  h.feed(radio::channelInfo(0, "Public"));
  h.feed(radio::channelInfo(1, ""));
  EXPECT_EQ(h.client.stage(), Stage::Sync);

  h.feed(radio::channelMessageV3(0, "Ana: hello", 1790000001));
  EXPECT_EQ(h.client.stage(), Stage::Sync);
  EXPECT_EQ(h.readyCount, 0);
  h.feed(radio::noMoreMessages());

  EXPECT_TRUE(h.client.isReady());
  EXPECT_EQ(h.readyCount, 1);
  EXPECT_EQ(h.sentCodes(),
            (std::vector<uint8_t>{cmd::DEVICE_QUERY, cmd::APP_START, cmd::GET_DEVICE_TIME, cmd::GET_CONTACTS,
                                  cmd::GET_CHANNEL, cmd::GET_CHANNEL, cmd::SYNC_NEXT_MESSAGE, cmd::SYNC_NEXT_MESSAGE}));
  EXPECT_EQ(h.sent[4][1], 0);
  EXPECT_EQ(h.sent[5][1], 1);
  EXPECT_EQ(h.deviceTime, 1790000000u);
  EXPECT_EQ(h.contactsCount, 2u);
  EXPECT_EQ(h.contacts, (std::vector<std::string>{"Ana", "Relay 7"}));
  EXPECT_EQ(h.contactsEnd, 250u);
  EXPECT_EQ(h.channels, (std::vector<std::string>{"Public"}));
  EXPECT_EQ(h.messages, (std::vector<std::string>{"Ana: hello"}));
  EXPECT_EQ(h.client.deviceInfo().maxChannels, 2);
  EXPECT_STREQ(h.client.selfInfo().name, "Trailhead");
}

TEST(MeshClientHandshake, SendsAppNameAndContactsSince) {
  Harness h;
  MeshCompanionClient::StartOptions options;
  options.appName = "Reader";
  options.contactsSince = 0x0A0B0C0D;
  h.client.start(options, h.now);
  h.feed(radio::deviceInfo(13, 50, 1));
  ASSERT_EQ(h.lastCmd(), cmd::APP_START);
  EXPECT_EQ(std::string(h.sent.back().begin() + 8, h.sent.back().end()), "Reader");
  h.feed(radio::selfInfo("Me"));
  h.feed(radio::currTime(1));
  ASSERT_EQ(h.lastCmd(), cmd::GET_CONTACTS);
  EXPECT_EQ(h.sent.back(), (radio::Frame{cmd::GET_CONTACTS, 0x0D, 0x0C, 0x0B, 0x0A}));
}

TEST(MeshClientHandshake, SetsRadioClockOnlyWhenAskedAndAddsElapsedTime) {
  Harness h;
  MeshCompanionClient::StartOptions options;
  options.setTimeEpoch = 1790000000;
  h.client.start(options, h.now);
  h.feed(radio::deviceInfo(13, 50, 1));
  h.feed(radio::selfInfo("Me"));
  h.now += 3000;  // the handshake has taken three seconds so far
  h.feed(radio::currTime(1700000000));
  ASSERT_EQ(h.lastCmd(), cmd::SET_DEVICE_TIME);
  EXPECT_EQ(readU32(&h.sent.back()[1]), 1790000003u);
  h.feed(radio::ok());
  EXPECT_EQ(h.client.stage(), Stage::Contacts);

  Harness untouched;
  untouched.handshake();
  EXPECT_EQ(untouched.failures.size(), 0u);
}

TEST(MeshClientHandshake, RadioRefusingTheClockDoesNotStopTheHandshake) {
  Harness h;
  MeshCompanionClient::StartOptions options;
  options.setTimeEpoch = 5;
  h.client.start(options, h.now);
  h.feed(radio::deviceInfo(13, 50, 1));
  h.feed(radio::selfInfo("Me"));
  h.feed(radio::currTime(1790000000));
  h.feed(radio::error(err::ILLEGAL_ARG));
  EXPECT_EQ(h.client.stage(), Stage::Contacts);
  EXPECT_EQ(h.handshakeFailed, 0);
}

TEST(MeshClientHandshake, SilentRadioIsRetriedThenReportedAsFailed) {
  Harness h;
  h.client.start({}, h.now);
  for (int attempt = 0; attempt < MeshCompanionClient::HANDSHAKE_RETRIES; attempt++) {
    h.advance(MeshCompanionClient::COMMAND_TIMEOUT_MS);
    EXPECT_EQ(h.client.stage(), Stage::DeviceQuery);
    EXPECT_EQ(h.handshakeFailed, 0);
  }
  EXPECT_EQ(h.sent.size(), 3u);
  h.advance(MeshCompanionClient::COMMAND_TIMEOUT_MS);
  EXPECT_EQ(h.client.stage(), Stage::Failed);
  EXPECT_EQ(h.handshakeFailed, 1);
  h.advance(60000);
  EXPECT_EQ(h.sent.size(), 3u);  // nothing more is sent until start()
}

TEST(MeshClientHandshake, DoesNotTimeOutJustBeforeTheDeadline) {
  Harness h;
  h.client.start({}, h.now);
  h.advance(MeshCompanionClient::COMMAND_TIMEOUT_MS - 1);
  EXPECT_EQ(h.sent.size(), 1u);
  h.feed(radio::deviceInfo(13, 50, 1));
  EXPECT_EQ(h.client.stage(), Stage::AppStart);
}

TEST(MeshClientHandshake, ChannelScanStopsAtFirstErrorAndUsesFallbackCount) {
  Harness h;
  h.client.start({}, h.now);
  h.feed(radio::deviceInfo(2, 0, 0));  // old firmware: no channel count
  h.feed(radio::selfInfo("Me"));
  h.feed(radio::currTime(1));
  h.feed(radio::endOfContacts(0));
  h.feed(radio::channelInfo(0, "Public"));
  h.feed(radio::error(err::NOT_FOUND));
  EXPECT_EQ(h.client.stage(), Stage::Sync);

  Harness full;
  full.client.start({}, full.now);
  full.feed(radio::deviceInfo(2, 0, 0));
  full.feed(radio::selfInfo("Me"));
  full.feed(radio::currTime(1));
  full.feed(radio::endOfContacts(0));
  for (uint8_t i = 0; i < MeshCompanionClient::FALLBACK_CHANNEL_SLOTS; i++) {
    EXPECT_EQ(full.client.stage(), Stage::Channels);
    full.feed(radio::channelInfo(i, "c"));
  }
  EXPECT_EQ(full.client.stage(), Stage::Sync);
}

TEST(MeshClientHandshake, ChannelReadIsRetriedAfterATimeout) {
  Harness h;
  h.client.start({}, h.now);
  h.feed(radio::deviceInfo(13, 50, 2));
  h.feed(radio::selfInfo("Me"));
  h.feed(radio::currTime(1));
  h.feed(radio::endOfContacts(0));
  h.feed(radio::channelInfo(0, "Public"));
  ASSERT_EQ(h.client.stage(), Stage::Channels);

  h.sent.clear();
  h.advance(MeshCompanionClient::COMMAND_TIMEOUT_MS);
  h.advance(1);
  ASSERT_EQ(h.lastCmd(), cmd::GET_CHANNEL);
  EXPECT_EQ(h.sent.back()[1], 1);  // the same slot again, not the next one
  h.feed(radio::channelInfo(1, "Hikers"));
  EXPECT_EQ(h.client.stage(), Stage::Sync);
  EXPECT_EQ(h.channels, (std::vector<std::string>{"Public", "Hikers"}));
  EXPECT_EQ(h.handshakeFailed, 0);
}

TEST(MeshClientHandshake, SilentChannelReadFailsTheHandshake) {
  Harness h;
  h.client.start({}, h.now);
  h.feed(radio::deviceInfo(13, 50, 2));
  h.feed(radio::selfInfo("Me"));
  h.feed(radio::currTime(1));
  h.feed(radio::endOfContacts(0));
  ASSERT_EQ(h.client.stage(), Stage::Channels);

  for (uint8_t attempt = 0; attempt <= MeshCompanionClient::HANDSHAKE_RETRIES; attempt++) {
    EXPECT_EQ(h.handshakeFailed, 0);
    h.advance(MeshCompanionClient::COMMAND_TIMEOUT_MS);
    h.advance(1);
  }
  EXPECT_EQ(h.client.stage(), Stage::Failed);
  EXPECT_EQ(h.handshakeFailed, 1);
  EXPECT_EQ(h.readyCount, 0);
}

TEST(MeshClientHandshake, ContactListKeepsStreamingPastTheBaseTimeout) {
  Harness h;
  h.client.start({}, h.now);
  h.feed(radio::deviceInfo(13, 50, 1));
  h.feed(radio::selfInfo("Me"));
  h.feed(radio::currTime(1));
  h.feed(radio::contactsStart(4));
  for (int i = 0; i < 4; i++) {
    h.advance(MeshCompanionClient::COMMAND_TIMEOUT_MS - 500);
    h.feed(radio::contact("slow", static_cast<uint8_t>(i), 10 + i));
  }
  EXPECT_EQ(h.client.stage(), Stage::Contacts);
  EXPECT_EQ(h.contacts.size(), 4u);
  h.feed(radio::endOfContacts(13));
  EXPECT_EQ(h.client.stage(), Stage::Channels);
}

TEST(MeshClientHandshake, StalledContactListMovesOn) {
  Harness h;
  h.client.start({}, h.now);
  h.feed(radio::deviceInfo(13, 50, 1));
  h.feed(radio::selfInfo("Me"));
  h.feed(radio::currTime(1));
  h.feed(radio::contactsStart(4));
  h.advance(MeshCompanionClient::COMMAND_TIMEOUT_MS);
  EXPECT_EQ(h.client.stage(), Stage::Channels);
}

TEST(MeshClientHandshake, WorksAcrossMillisecondClockWrap) {
  Harness h;
  h.now = 0xFFFFFF00u;
  h.client.start({}, h.now);
  h.advance(0x200);  // clock wraps; only 512 ms have passed
  EXPECT_EQ(h.sent.size(), 1u);
  EXPECT_EQ(h.client.stage(), Stage::DeviceQuery);
  h.advance(MeshCompanionClient::COMMAND_TIMEOUT_MS);
  EXPECT_EQ(h.sent.size(), 2u);
}

TEST(MeshClientHandshake, LinkBusyAtStartIsRetriedOnPoll) {
  Harness h;
  h.linkBusy = true;
  h.client.start({}, h.now);
  EXPECT_TRUE(h.sent.empty());
  h.linkBusy = false;
  h.advance(10);
  EXPECT_EQ(h.lastCmd(), cmd::DEVICE_QUERY);
}

TEST(MeshClientCommands, RejectedUntilHandshakeCompletes) {
  Harness h;
  EXPECT_FALSE(h.client.sendDirect(PEER, "hi", 2, 1, 0, 1));
  h.client.start({}, h.now);
  EXPECT_FALSE(h.client.sendDirect(PEER, "hi", 2, 1, 0, 1));
  EXPECT_FALSE(h.client.requestBattery());
}

TEST(MeshClientCommands, DirectMessageIsAcceptedThenConfirmed) {
  Harness h;
  h.handshake();
  ASSERT_TRUE(h.client.sendDirect(PEER, "on my way", 9, 1790000500, 0, 42));
  h.advance(0);
  ASSERT_EQ(h.lastCmd(), cmd::SEND_TXT_MSG);
  EXPECT_EQ(memcmp(&h.sent.back()[7], PEER, KEY_PREFIX_SIZE), 0);

  h.feed(radio::sent(false, 0xABCD1234, 8000));
  EXPECT_EQ(h.directAccepted, (std::vector<uint32_t>{42}));
  EXPECT_EQ(h.lastSent.expectedAck, 0xABCD1234u);
  EXPECT_EQ(h.lastSent.timeoutMs, 8000u);
  EXPECT_TRUE(h.confirmedAcks.empty());

  h.feed(radio::sendConfirmed(0xABCD1234, 1900));
  EXPECT_EQ(h.confirmedAcks, (std::vector<uint32_t>{0xABCD1234}));
  EXPECT_TRUE(h.failures.empty());
}

TEST(MeshClientCommands, ChannelMessageAcceptsOkOrSent) {
  Harness h;
  h.handshake();
  ASSERT_TRUE(h.client.sendChannel(0, "hello all", 9, 1, 7));
  h.advance(0);
  ASSERT_EQ(h.lastCmd(), cmd::SEND_CHANNEL_TXT_MSG);
  h.feed(radio::ok());
  ASSERT_TRUE(h.client.sendChannel(0, "again", 5, 2, 8));
  h.advance(0);
  h.feed(radio::sent(true, 0, 0));
  EXPECT_EQ(h.channelAccepted, (std::vector<uint32_t>{7, 8}));
  EXPECT_TRUE(h.directAccepted.empty());
}

TEST(MeshClientCommands, ChannelTextLimitUsesNodeNameLength) {
  Harness h;
  h.handshake(1, "Trailhead");  // 9 bytes, so 160 - 9 - 2 = 149 bytes of text
  const std::string fits(149, 'a');
  const std::string tooLong(150, 'a');
  EXPECT_TRUE(h.client.sendChannel(0, fits.data(), fits.size(), 1, 1));
  EXPECT_FALSE(h.client.sendChannel(0, tooLong.data(), tooLong.size(), 1, 2));
}

TEST(MeshClientCommands, RadioErrorIsReportedWithItsCode) {
  Harness h;
  h.handshake();
  ASSERT_TRUE(h.client.sendDirect(PEER, "x", 1, 1, 0, 9));
  h.advance(0);
  h.feed(radio::error(err::TABLE_FULL));
  ASSERT_EQ(h.failures.size(), 1u);
  EXPECT_EQ(h.failures[0].cmd, cmd::SEND_TXT_MSG);
  EXPECT_EQ(h.failures[0].tag, 9u);
  EXPECT_EQ(h.failures[0].reason, FailReason::RadioError);
  EXPECT_EQ(h.failures[0].errCode, err::TABLE_FULL);
  EXPECT_TRUE(h.directAccepted.empty());
}

TEST(MeshClientCommands, TimeoutFailsTheCommandAndSendsTheNext) {
  Harness h;
  h.handshake();
  ASSERT_TRUE(h.client.sendDirect(PEER, "first", 5, 1, 0, 1));
  ASSERT_TRUE(h.client.sendDirect(PEER, "second", 6, 2, 0, 2));
  h.advance(0);
  EXPECT_EQ(h.sent.size(), 1u);  // one in flight at a time
  h.advance(MeshCompanionClient::COMMAND_TIMEOUT_MS);
  ASSERT_EQ(h.failures.size(), 1u);
  EXPECT_EQ(h.failures[0].tag, 1u);
  EXPECT_EQ(h.failures[0].reason, FailReason::Timeout);
  ASSERT_EQ(h.sent.size(), 2u);
  h.feed(radio::sent(true, 5, 100));
  EXPECT_EQ(h.directAccepted, (std::vector<uint32_t>{2}));
}

TEST(MeshClientCommands, QueueHoldsCapacityAndPreservesOrder) {
  Harness h;
  h.handshake();
  for (uint32_t i = 0; i < MeshCompanionClient::QUEUE_CAPACITY; i++) {
    EXPECT_TRUE(h.client.sendDirect(PEER, "m", 1, i, 0, i)) << i;
  }
  EXPECT_FALSE(h.client.sendDirect(PEER, "m", 1, 99, 0, 99));
  EXPECT_FALSE(h.client.isIdle());
  for (uint32_t i = 0; i < MeshCompanionClient::QUEUE_CAPACITY; i++) {
    h.advance(0);
    h.feed(radio::sent(false, i + 1, 100));
  }
  std::vector<uint32_t> expected;
  for (uint32_t i = 0; i < MeshCompanionClient::QUEUE_CAPACITY; i++) expected.push_back(i);
  EXPECT_EQ(h.directAccepted, expected);
  EXPECT_TRUE(h.client.isIdle());
  EXPECT_TRUE(h.client.sendDirect(PEER, "m", 1, 1, 0, 1));
}

TEST(MeshClientCommands, BusyLinkKeepsTheCommandQueued) {
  Harness h;
  h.handshake();
  h.linkBusy = true;
  ASSERT_TRUE(h.client.sendDirect(PEER, "wait", 4, 1, 0, 3));
  h.advance(10);
  h.advance(10);
  EXPECT_TRUE(h.sent.empty());
  EXPECT_TRUE(h.failures.empty());
  h.linkBusy = false;
  h.advance(10);
  ASSERT_EQ(h.lastCmd(), cmd::SEND_TXT_MSG);
  h.feed(radio::sent(false, 1, 1));
  EXPECT_EQ(h.directAccepted, (std::vector<uint32_t>{3}));
}

TEST(MeshClientCommands, ResetFailsEverythingPendingWithLinkDown) {
  Harness h;
  h.handshake();
  ASSERT_TRUE(h.client.sendDirect(PEER, "a", 1, 1, 0, 11));
  ASSERT_TRUE(h.client.sendChannel(0, "b", 1, 1, 12));
  h.advance(0);  // first is in flight, second still queued
  h.client.reset();
  ASSERT_EQ(h.failures.size(), 2u);
  EXPECT_EQ(h.failures[0].tag, 11u);
  EXPECT_EQ(h.failures[1].tag, 12u);
  EXPECT_EQ(h.failures[0].reason, FailReason::LinkDown);
  EXPECT_EQ(h.failures[1].reason, FailReason::LinkDown);
  EXPECT_EQ(h.client.stage(), Stage::Idle);
  h.feed(radio::sent(false, 1, 1));  // a late reply after reset is ignored
  EXPECT_TRUE(h.directAccepted.empty());
}

TEST(MeshClientCommands, SimpleCommandsCompleteOnOk) {
  Harness h;
  h.handshake();
  uint8_t key[PUB_KEY_SIZE] = {7};
  ASSERT_TRUE(h.client.sendSelfAdvert(true));
  ASSERT_TRUE(h.client.setAdvertName("New Name"));
  ASSERT_TRUE(h.client.setTxPower(14));
  ASSERT_TRUE(h.client.removeContact(key));
  ASSERT_TRUE(h.client.resetPath(key));
  for (int i = 0; i < 5; i++) {
    h.advance(0);
    h.feed(radio::ok());
  }
  EXPECT_EQ(h.done, (std::vector<uint8_t>{cmd::SEND_SELF_ADVERT, cmd::SET_ADVERT_NAME, cmd::SET_RADIO_TX_POWER,
                                          cmd::REMOVE_CONTACT, cmd::RESET_PATH}));
  EXPECT_EQ(h.sent[0], (radio::Frame{cmd::SEND_SELF_ADVERT, 1}));
  EXPECT_EQ(h.sent[2], (radio::Frame{cmd::SET_RADIO_TX_POWER, 14}));
}

TEST(MeshClientCommands, ContactIsReadThenWrittenBack) {
  Harness h;
  h.handshake();
  uint8_t key[PUB_KEY_SIZE];
  memset(key, 0xC3, sizeof(key));

  ASSERT_TRUE(h.client.requestContact(key, 41));
  h.advance(0);
  ASSERT_EQ(h.lastCmd(), cmd::GET_CONTACT_BY_KEY);
  h.feed(radio::contact("Ana", 0xC3, 77, adv::CHAT, 2));
  EXPECT_EQ(h.fetchedTag, 41u);
  EXPECT_STREQ(h.fetched.name, "Ana");
  EXPECT_EQ(h.fetched.outPathLen, 2);
  EXPECT_TRUE(h.contacts.empty());  // not taken for a contact list entry

  Contact changed = h.fetched;
  changed.flags |= CONTACT_FLAG_FAVORITE;
  ASSERT_TRUE(h.client.updateContact(changed, 42));
  h.advance(0);
  ASSERT_EQ(h.lastCmd(), cmd::ADD_UPDATE_CONTACT);
  EXPECT_EQ(h.sent.back()[1 + PUB_KEY_SIZE + 1], CONTACT_FLAG_FAVORITE);
  h.feed(radio::ok());
  EXPECT_EQ(h.done, (std::vector<uint8_t>{cmd::ADD_UPDATE_CONTACT}));
  EXPECT_TRUE(h.failures.empty());
}

TEST(MeshClientCommands, ContactReadIgnoresAnotherContactsFrame) {
  Harness h;
  h.handshake();
  uint8_t key[PUB_KEY_SIZE];
  memset(key, 0xC3, sizeof(key));
  ASSERT_TRUE(h.client.requestContact(key, 41));
  h.advance(0);

  // A frame left over from a contact list, or a late answer to another read.
  h.feed(radio::contact("Somebody else", 0xD4, 77));
  EXPECT_EQ(h.fetchedTag, 0u);
  EXPECT_TRUE(h.failures.empty());

  h.feed(radio::contact("Ana", 0xC3, 77));
  EXPECT_EQ(h.fetchedTag, 41u);
  EXPECT_STREQ(h.fetched.name, "Ana");
}

TEST(MeshClientCommands, ContactReadWithOnlyWrongFramesTimesOut) {
  Harness h;
  h.handshake();
  uint8_t key[PUB_KEY_SIZE];
  memset(key, 0xC3, sizeof(key));
  ASSERT_TRUE(h.client.requestContact(key, 41));
  h.advance(0);
  h.feed(radio::contact("Somebody else", 0xD4, 77));
  h.advance(MeshCompanionClient::COMMAND_TIMEOUT_MS);
  ASSERT_EQ(h.failures.size(), 1u);
  EXPECT_EQ(h.failures[0].tag, 41u);
  EXPECT_EQ(h.failures[0].reason, FailReason::Timeout);
  EXPECT_EQ(h.fetchedTag, 0u);
}

TEST(MeshClientCommands, RefusedContactWriteIsReported) {
  Harness h;
  h.handshake();
  Contact contact;
  memset(contact.publicKey, 0xC3, sizeof(contact.publicKey));
  ASSERT_TRUE(h.client.updateContact(contact, 42));
  h.advance(0);
  h.feed(radio::error(err::TABLE_FULL));
  ASSERT_EQ(h.failures.size(), 1u);
  EXPECT_EQ(h.failures[0].cmd, cmd::ADD_UPDATE_CONTACT);
  EXPECT_EQ(h.failures[0].tag, 42u);
  EXPECT_TRUE(h.done.empty());
}

TEST(MeshClientCommands, UnknownContactFailsTheRead) {
  Harness h;
  h.handshake();
  uint8_t key[PUB_KEY_SIZE] = {0};
  ASSERT_TRUE(h.client.requestContact(key, 41));
  h.advance(0);
  h.feed(radio::error(err::NOT_FOUND));
  ASSERT_EQ(h.failures.size(), 1u);
  EXPECT_EQ(h.failures[0].cmd, cmd::GET_CONTACT_BY_KEY);
  EXPECT_EQ(h.failures[0].tag, 41u);
  EXPECT_EQ(h.fetchedTag, 0u);
}

TEST(MeshClientCommands, BatteryRequest) {
  Harness h;
  h.handshake();
  ASSERT_TRUE(h.client.requestBattery());
  h.advance(0);
  h.feed(radio::battery(3987, 10, 2048));
  EXPECT_EQ(h.lastBattery.milliVolts, 3987);
  EXPECT_EQ(h.lastBattery.totalKb, 2048u);
}

TEST(MeshClientCommands, ReplyOfTheWrongKindIsIgnoredUntilTheRightOneArrives) {
  Harness h;
  h.handshake();
  ASSERT_TRUE(h.client.sendDirect(PEER, "x", 1, 1, 0, 5));
  h.advance(0);
  h.feed(radio::ok());  // not a valid answer to SEND_TXT_MSG
  h.feed(radio::battery(4000, 0, 0));
  EXPECT_TRUE(h.directAccepted.empty());
  EXPECT_TRUE(h.failures.empty());
  h.feed(radio::sent(false, 77, 500));
  EXPECT_EQ(h.directAccepted, (std::vector<uint32_t>{5}));
}

TEST(MeshClientPushes, MessageWaitingDrainsTheRadioQueue) {
  Harness h;
  h.handshake();
  h.feed(radio::pushCode(push::MSG_WAITING));
  ASSERT_EQ(h.lastCmd(), cmd::SYNC_NEXT_MESSAGE);
  h.feed(radio::contactMessageV3(0xC3, "one", 1));
  h.feed(radio::channelMessageV3(0, "Ana: two", 2));
  EXPECT_EQ(h.sent.size(), 3u);
  h.feed(radio::noMoreMessages());
  EXPECT_EQ(h.messages, (std::vector<std::string>{"one", "Ana: two"}));
  h.advance(1000);
  EXPECT_EQ(h.sent.size(), 3u);  // drained: no further polling
  EXPECT_TRUE(h.client.isIdle());
}

TEST(MeshClientPushes, MessageWaitingDuringACommandDoesNotCompleteIt) {
  Harness h;
  h.handshake();
  ASSERT_TRUE(h.client.sendDirect(PEER, "x", 1, 1, 0, 5));
  h.advance(0);
  h.feed(radio::pushCode(push::MSG_WAITING));
  EXPECT_EQ(h.sent.size(), 1u);
  EXPECT_TRUE(h.directAccepted.empty());
  h.feed(radio::sent(false, 1, 1));
  EXPECT_EQ(h.directAccepted, (std::vector<uint32_t>{5}));
  EXPECT_EQ(h.lastCmd(), cmd::SYNC_NEXT_MESSAGE);  // the sync runs once the command is done
}

TEST(MeshClientPushes, UserCommandsGoBeforeBackgroundSync) {
  Harness h;
  h.handshake();
  h.linkBusy = true;
  h.feed(radio::pushCode(push::MSG_WAITING));
  ASSERT_TRUE(h.client.sendDirect(PEER, "x", 1, 1, 0, 5));
  h.linkBusy = false;
  h.advance(0);
  EXPECT_EQ(h.lastCmd(), cmd::SEND_TXT_MSG);
  h.feed(radio::sent(false, 1, 1));
  EXPECT_EQ(h.lastCmd(), cmd::SYNC_NEXT_MESSAGE);
}

TEST(MeshClientPushes, BinaryDatagramKeepsTheSyncLoopGoing) {
  Harness h;
  h.handshake();
  h.feed(radio::pushCode(push::MSG_WAITING));
  h.feed(radio::Frame{resp::CHANNEL_DATA_RECV, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 1, 0x42});
  EXPECT_EQ(h.sent.size(), 2u);
  EXPECT_TRUE(h.messages.empty());
  h.feed(radio::noMoreMessages());
  EXPECT_TRUE(h.client.isIdle());
}

TEST(MeshClientPushes, AdvertRefreshesContactsSinceLastSync) {
  Harness h;
  h.client.start({}, h.now);
  h.feed(radio::deviceInfo(13, 50, 1));
  h.feed(radio::selfInfo("Me"));
  h.feed(radio::currTime(1));
  h.feed(radio::contactsStart(1));
  h.feed(radio::contact("Ana", 0xC3, 500));
  h.feed(radio::endOfContacts(500));
  h.feed(radio::channelInfo(0, "Public"));
  h.feed(radio::noMoreMessages());
  ASSERT_TRUE(h.client.isReady());
  h.sent.clear();

  h.feed(radio::pushCode(push::ADVERT));
  ASSERT_EQ(h.sent.size(), 1u);
  EXPECT_EQ(h.sent[0][0], cmd::GET_CONTACTS);
  EXPECT_EQ(readU32(&h.sent[0][1]), 500u);
  h.feed(radio::contactsStart(1));
  h.feed(radio::contact("Ben", 0xD4, 600));
  h.feed(radio::endOfContacts(600));
  EXPECT_EQ(h.contacts, (std::vector<std::string>{"Ana", "Ben"}));
  EXPECT_EQ(h.contactsEnd, 600u);

  h.feed(radio::pushCode(push::PATH_UPDATED));
  EXPECT_EQ(readU32(&h.sent.back()[1]), 600u);
}

TEST(MeshClientPushes, EndOfContactsNeverMovesSinceBackwards) {
  Harness h;
  MeshCompanionClient::StartOptions options;
  options.contactsSince = 900;
  h.client.start(options, h.now);
  h.feed(radio::deviceInfo(13, 50, 1));
  h.feed(radio::selfInfo("Me"));
  h.feed(radio::currTime(1));
  h.feed(radio::endOfContacts(0));  // nothing changed since 900
  EXPECT_EQ(h.contactsEnd, 900u);
}

TEST(MeshClientPushes, DiscoveredContactDeletedAndFull) {
  Harness h;
  h.handshake();
  h.feed(radio::contact("Stranger", 0x11, 1, adv::CHAT, -1, push::NEW_ADVERT));
  radio::Frame deleted = {push::CONTACT_DELETED};
  radio::putBytes(deleted, 0x22, PUB_KEY_SIZE);
  h.feed(deleted);
  h.feed(radio::pushCode(push::CONTACTS_FULL));
  EXPECT_EQ(h.discovered, (std::vector<std::string>{"Stranger"}));
  EXPECT_EQ(h.contactDeleted, 1);
  EXPECT_EQ(h.contactsFull, 1);
  EXPECT_TRUE(h.sent.empty());
}

TEST(MeshClientPushes, UnknownPushAndStrayFramesAreIgnored) {
  Harness h;
  h.handshake();
  h.feed(radio::pushCode(0x88));                     // RF log data
  h.feed(radio::ok());                               // reply with nothing in flight
  h.feed(radio::Frame{});                            // empty frame
  h.feed(radio::Frame{push::SEND_CONFIRMED, 1, 2});  // truncated push
  EXPECT_TRUE(h.sent.empty());
  EXPECT_TRUE(h.confirmedAcks.empty());
  EXPECT_TRUE(h.client.isIdle());
}

TEST(MeshClientLifecycle, RestartAfterResetRunsAFreshHandshake) {
  Harness h;
  h.handshake();
  h.client.reset();
  h.handshake(2);
  EXPECT_EQ(h.readyCount, 2);
  EXPECT_EQ(h.client.deviceInfo().maxChannels, 2);
}
